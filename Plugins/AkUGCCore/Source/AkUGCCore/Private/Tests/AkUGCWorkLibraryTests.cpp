#include "Misc/AutomationTest.h"
#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Pack/AkUGCWorkLibrary.h"
#include "Pack/AkUGCLogicPackCodec.h"
#include "Pack/AkUGCLogicPackHasher.h"
#include "Pack/AkUGCLogicPackSignature.h"
#include "Pack/AkUGCPublisherKeyStore.h"
#include "Prefab/AkUGCOfficialPrefabCatalog.h"
#include "Prefab/AkUGCPrefabRegistry.h"
#include "Template/AkUGCTowerDefenseTemplate.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAkUGCWorkLibraryTest, "AkUGC.Core.WorkLibrary.PublishImportAndIsolation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ServerContext | EAutomationTestFlags::EngineFilter)
bool FAkUGCWorkLibraryTest::RunTest(const FString& Parameters)
{
    const FString Root = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation/WorkLibrary"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
#if !PLATFORM_WINDOWS
    FString UnsupportedPublicKey, UnsupportedError;
    TestFalse(TEXT("Non-Windows publisher identity is explicitly unsupported"),
        FAkUGCPublisherKeyStore(FPaths::Combine(Root, TEXT("Publisher"))).GetOrCreatePublicKey(
            UnsupportedPublicKey, &UnsupportedError));
    TestTrue(TEXT("Unsupported platform reports a clear boundary"), UnsupportedError.Contains(TEXT("Win64 DPAPI")));
    IFileManager::Get().DeleteDirectory(*Root, false, true);
    return true;
#endif
    FAkUGCWorkLibrary Author(FPaths::Combine(Root, TEXT("Author"))), Receiver(FPaths::Combine(Root, TEXT("Receiver")));
    FString PublisherPublicKey, ReusedPublicKey, Error, Share;
    const FString PublisherDirectory = FPaths::Combine(Root, TEXT("Publisher"));
    const FAkUGCPublisherKeyStore Publisher(PublisherDirectory);
    TestTrue(TEXT("Protected publisher identity creates"), Publisher.GetOrCreatePublicKey(PublisherPublicKey, &Error));
    FString IdentityBefore, IdentityAfter;
    TestTrue(TEXT("Protected identity file is readable"), FFileHelper::LoadFileToString(IdentityBefore, *Publisher.IdentityPath()));
    FString CompatibilityFence;
    TestTrue(TEXT("Legacy path contains a non-secret rollback fence"), FFileHelper::LoadFileToString(
        CompatibilityFence, *Publisher.LegacyPrivateKeyPath()));
    TestTrue(TEXT("Rollback fence declares that it contains no private key"),
        CompatibilityFence.Contains(TEXT("\"containsPrivateKey\":false")));
    TestFalse(TEXT("Identity envelope does not label plaintext private material"), IdentityBefore.Contains(TEXT("\"privateKey\"")));
    TestTrue(TEXT("Publisher identity persists"), Publisher.GetOrCreatePublicKey(ReusedPublicKey, &Error));
    TestEqual(TEXT("Public identity is not rotated silently"), PublisherPublicKey, ReusedPublicKey);
    TestTrue(TEXT("Protected identity remains readable after restart"), FFileHelper::LoadFileToString(IdentityAfter, *Publisher.IdentityPath()));
    TestEqual(TEXT("Reading identity does not rewrite ciphertext"), IdentityAfter, IdentityBefore);
    FAkUGCPrefabRegistry Registry;
    FAkUGCProjectDocument Document;
    FAkUGCOfficialPrefabCatalog::RegisterTowerDefense(Registry);
    FAkUGCTowerDefenseTemplate::CreateDefaultDocument(Registry, Document);
    Document.Manifest.DisplayName = TEXT("独立分享作品");
    const FString SourceHash = FAkUGCLogicPackHasher::HashDocument(Document);
    FAkUGCWorkEntry Published, Imported;
    if (!Author.PublishWithPublisherIdentity(Document, PublisherDirectory,
        FPaths::Combine(Root, TEXT("Shares")), Published, Share, &Error))
    { AddError(Error); IFileManager::Get().DeleteDirectory(*Root, false, true); return false; }
    FString ShareJson;
    FFileHelper::LoadFileToString(ShareJson, *Share);
    TestFalse(TEXT("Share does not contain protected private material"), ShareJson.Contains(TEXT("protectedPrivateKey")));
    TestEqual(TEXT("Published signer matches protected identity"), Published.TrustedPublicKey, PublisherPublicKey);
    TestEqual(TEXT("Publishing does not edit author document"), FAkUGCLogicPackHasher::HashDocument(Document), SourceHash);
    TestFalse(TEXT("Embedded signer does not imply trust"), Receiver.Import(Share, TEXT(""), Imported, &Error));
    FAkUGCLogicPackKeyPair Attacker;
    FAkUGCLogicPackSigner::GenerateKeyPair(Attacker);
    TestFalse(TEXT("Wrong independently trusted key rejected"), Receiver.Import(Share, Attacker.PublicKey, Imported, &Error));
    TestTrue(TEXT("Independently trusted receiver imports"), Receiver.Import(Share, Published.TrustedPublicKey, Imported, &Error));
    TestTrue(TEXT("Duplicate import idempotent"), Receiver.Import(Share, Published.TrustedPublicKey, Imported, &Error));
    FAkUGCWorkLibrary Restarted(FPaths::Combine(Root, TEXT("Receiver")));
    const auto Loaded = Restarted.Load(Published.ReleaseId);
    TestTrue(TEXT("Restarted library verifies stored trust and pack"), Loaded.bSucceeded);
    TestEqual(TEXT("Receiver retains exact authored content"), FAkUGCLogicPackHasher::HashDocument(Loaded.Pack.Document), SourceHash);
    TArray<FAkUGCWorkEntry> Entries;
    int32 Rejected = 0;
    Restarted.List(Entries, Rejected);
    TestEqual(TEXT("Duplicate import remains single entry"), Entries.Num(), 1);
    const FString Export = FPaths::Combine(Root, TEXT("Exports/share.akugc"));
    TestTrue(TEXT("Export re-verifies and writes pack"), Restarted.Export(Published.ReleaseId, Export, &Error));
    TestFalse(TEXT("Export will not overwrite existing file"), Restarted.Export(Published.ReleaseId, Export, &Error));
    const FString Blocked = FPaths::Combine(Root, TEXT("blocked"));
    FFileHelper::SaveStringToFile(TEXT("not a directory"), *Blocked);
    FAkUGCWorkEntry Failed;
    FString FailedShare;
    TestFalse(TEXT("Unwritable share path fails publish"), Author.PublishWithPublisherIdentity(
        Document, PublisherDirectory, Blocked, Failed, FailedShare, &Error));
    Author.List(Entries, Rejected);
    TestEqual(TEXT("Failed publish preserves original library only"), Entries.Num(), 1);
    FAkUGCWorkLibrary BlockedLibrary(Blocked);
    TestFalse(TEXT("Library commit failure fails publish"), BlockedLibrary.PublishWithPublisherIdentity(
        Document, PublisherDirectory, FPaths::Combine(Root, TEXT("Rollback")), Failed, FailedShare, &Error));
    TArray<FString> RemainingShares;
    IFileManager::Get().FindFiles(RemainingShares, *FPaths::Combine(Root, TEXT("Rollback/*.akugc")), true, false);
    TestEqual(TEXT("Failed library commit rolls back own share"), RemainingShares.Num(), 0);
    FAkUGCLogicPack Mutated = Loaded.Pack;
    Mutated.Document.Manifest.DisplayName = TEXT("篡改作品");
    FString Tampered;
    FAkUGCLogicPackCodec::Serialize(Mutated, Tampered);
    const FString BadFile = FPaths::Combine(Root, TEXT("tampered.akugc"));
    FFileHelper::SaveStringToFile(Tampered, *BadFile);
    TestFalse(TEXT("Modified document rejected on import"), Receiver.Import(BadFile, Published.TrustedPublicKey, Imported, &Error));
    Mutated = Loaded.Pack;
    FAkUGCLogicPackSigner::Sign(Mutated.Manifest, Attacker.PrivateKey, Mutated.Signature);
    FAkUGCLogicPackCodec::Serialize(Mutated, Tampered);
    FFileHelper::SaveStringToFile(Tampered, *BadFile);
    TestFalse(TEXT("Self-consistent re-signing by untrusted key rejected"), Receiver.Import(BadFile, Published.TrustedPublicKey, Imported, &Error));
    Mutated = Loaded.Pack;
    Mutated.Manifest.TemplateId = TEXT("conflicting.template");
    TestTrue(TEXT("Protected identity can sign without exporting private key"), Publisher.Sign(Mutated.Manifest, Mutated.Signature, &Error));
    FAkUGCLogicPackCodec::Serialize(Mutated, Tampered);
    FFileHelper::SaveStringToFile(Tampered, *BadFile);
    TestFalse(TEXT("Same ID with different trusted signed manifest rejected"), Receiver.Import(BadFile, Published.TrustedPublicKey, Imported, &Error));
    TestEqual(TEXT("Conflict preserves original manifest"), Restarted.Load(Published.ReleaseId).Pack.Manifest.TemplateId, Loaded.Pack.Manifest.TemplateId);
    TArray<uint8> Oversize;
    Oversize.SetNumZeroed(static_cast<int32>(FAkUGCWorkLibrary::MaxFileBytes + 1));
    FFileHelper::SaveArrayToFile(Oversize, *BadFile);
    TestFalse(TEXT("Oversized file rejected before JSON parsing"), Receiver.Import(BadFile, Published.TrustedPublicKey, Imported, &Error));
    FFileHelper::SaveStringToFile(TEXT("corrupt"), *Restarted.EntryPath(Published.ReleaseId));
    TestFalse(TEXT("Tampered local entry fails re-verification"), Restarted.Load(Published.ReleaseId).bSucceeded);
    Restarted.List(Entries, Rejected);
    TestEqual(TEXT("Corrupt entry not shown as playable"), Entries.Num(), 0);
    TestEqual(TEXT("Corrupt entry counted"), Rejected, 1);
    TestTrue(TEXT("Corrupt entry removable by safe GUID"), Restarted.Remove(Published.ReleaseId, &Error));
    TestTrue(TEXT("Deleting local record preserves external share"), IFileManager::Get().FileExists(*Share));
    TestFalse(TEXT("Invalid ID cannot resolve file"), Restarted.Load(FGuid()).bSucceeded);

    const FString MigrationDirectory = FPaths::Combine(Root, TEXT("MigrationPublisher"));
    const FAkUGCPublisherKeyStore MigrationPublisher(MigrationDirectory);
    FAkUGCLogicPackKeyPair LegacyIdentity;
    TestTrue(TEXT("Legacy fixture identity generates"), FAkUGCLogicPackSigner::GenerateKeyPair(LegacyIdentity, &Error));
    IFileManager::Get().MakeDirectory(*MigrationDirectory, true);
    TestTrue(TEXT("Legacy plaintext fixture writes"), FFileHelper::SaveStringToFile(
        LegacyIdentity.PrivateKey, *MigrationPublisher.LegacyPrivateKeyPath(),
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
    FString MigratedPublicKey;
    TestTrue(TEXT("Legacy plaintext identity migrates"), MigrationPublisher.GetOrCreatePublicKey(MigratedPublicKey, &Error));
    TestEqual(TEXT("Migration preserves publisher identity"), MigratedPublicKey, LegacyIdentity.PublicKey);
    FString MigratedFence;
    TestTrue(TEXT("Migration replaces legacy plaintext with rollback fence"), FFileHelper::LoadFileToString(
        MigratedFence, *MigrationPublisher.LegacyPrivateKeyPath()));
    TestTrue(TEXT("Migrated rollback fence contains no private key"),
        MigratedFence.Contains(TEXT("\"containsPrivateKey\":false")));
    FString MigratedEnvelope;
    TestTrue(TEXT("Migrated protected identity is readable"), FFileHelper::LoadFileToString(
        MigratedEnvelope, *MigrationPublisher.IdentityPath()));
    TestFalse(TEXT("Migrated envelope does not contain plaintext private key"), MigratedEnvelope.Contains(LegacyIdentity.PrivateKey));
    LegacyIdentity.PrivateKey.Reset();

    FString TamperedIdentity = IdentityBefore;
    const FString CiphertextField = TEXT("\"protectedPrivateKey\":\"");
    const int32 CiphertextIndex = TamperedIdentity.Find(CiphertextField) + CiphertextField.Len();
    TestTrue(TEXT("Protected identity contains ciphertext"), CiphertextIndex >= CiphertextField.Len());
    if (CiphertextIndex >= CiphertextField.Len() && CiphertextIndex < TamperedIdentity.Len())
    {
        TamperedIdentity[CiphertextIndex] = TamperedIdentity[CiphertextIndex] == TEXT('A') ? TEXT('B') : TEXT('A');
    }
    TestTrue(TEXT("Tampered identity fixture writes"), FFileHelper::SaveStringToFile(
        TamperedIdentity, *Publisher.IdentityPath(), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
    TestFalse(TEXT("DPAPI ciphertext tampering is not auto-replaced"), Publisher.GetOrCreatePublicKey(ReusedPublicKey, &Error));
    FString IdentityAfterRejection;
    TestTrue(TEXT("Rejected identity remains available for diagnosis"), FFileHelper::LoadFileToString(
        IdentityAfterRejection, *Publisher.IdentityPath()));
    TestEqual(TEXT("Rejected identity is not overwritten"), IdentityAfterRejection, TamperedIdentity);
    FString FenceAfterRejection;
    TestTrue(TEXT("Tampered identity keeps rollback fence"), FFileHelper::LoadFileToString(
        FenceAfterRejection, *Publisher.LegacyPrivateKeyPath()));
    TestEqual(TEXT("Tampered identity does not recreate plaintext storage"), FenceAfterRejection, CompatibilityFence);

    const FString ConcurrentDirectory = FPaths::Combine(Root, TEXT("ConcurrentPublisher"));
    TArray<TFuture<FString>> ConcurrentLoads;
    for (int32 Index = 0; Index < 8; ++Index)
    {
        ConcurrentLoads.Add(Async(EAsyncExecution::ThreadPool, [ConcurrentDirectory]()
        {
            FString PublicKey, LoadError;
            return FAkUGCPublisherKeyStore(ConcurrentDirectory).GetOrCreatePublicKey(PublicKey, &LoadError)
                ? PublicKey
                : TEXT("ERROR:") + LoadError;
        }));
    }
    FString ConcurrentPublicKey;
    for (TFuture<FString>& Load : ConcurrentLoads)
    {
        const FString LoadedPublicKey = Load.Get();
        TestFalse(TEXT("Concurrent identity load succeeds"), LoadedPublicKey.StartsWith(TEXT("ERROR:")));
        if (ConcurrentPublicKey.IsEmpty()) { ConcurrentPublicKey = LoadedPublicKey; }
        else { TestEqual(TEXT("Concurrent creation selects one identity"), LoadedPublicKey, ConcurrentPublicKey); }
    }

    const FString StaleDirectory = FPaths::Combine(Root, TEXT("StalePublisher"));
    const FAkUGCPublisherKeyStore StalePublisher(StaleDirectory);
    IFileManager::Get().MakeDirectory(*StaleDirectory, true);
    const FString StaleTemporary = StalePublisher.IdentityPath() + TEXT(".deadbeef.tmp");
    TestTrue(TEXT("Sensitive stale temporary fixture writes"), FFileHelper::SaveStringToFile(
        TEXT("protected credential fragment"), *StaleTemporary,
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
    TestFalse(TEXT("Sensitive stale temporary blocks identity creation"),
        StalePublisher.GetOrCreatePublicKey(ReusedPublicKey, &Error));
    TestFalse(TEXT("Blocked stale temporary does not create identity"),
        IFileManager::Get().FileExists(*StalePublisher.IdentityPath()));
    IFileManager::Get().DeleteDirectory(*Root, false, true);
    return true;
}
#endif

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

    const FTCHARToUTF8 RecoveryPassphraseUtf8(TEXT("correct horse battery staple"));
    TArray<uint8> RecoveryPassphrase;
    RecoveryPassphrase.Append(
        reinterpret_cast<const uint8*>(RecoveryPassphraseUtf8.Get()),
        RecoveryPassphraseUtf8.Length());
    const FTCHARToUTF8 WrongPassphraseUtf8(TEXT("wrong recovery password"));
    TArray<uint8> WrongPassphrase;
    WrongPassphrase.Append(
        reinterpret_cast<const uint8*>(WrongPassphraseUtf8.Get()),
        WrongPassphraseUtf8.Length());

    const FString RecoverySourceDirectory = FPaths::Combine(Root, TEXT("RecoverySource"));
    const FAkUGCPublisherKeyStore RecoverySource(RecoverySourceDirectory);
    FAkUGCPublisherIdentityInfo RecoverySourceInfo;
    TestTrue(TEXT("Recovery source identity creates"),
        RecoverySource.GetIdentityInfo(RecoverySourceInfo, &Error));
    const FString StaleRecoveryTarget =
        FPaths::Combine(Root, TEXT("Recovery/stale.akugcrecovery"));
    const FString StaleRecoveryTemporary =
        StaleRecoveryTarget + TEXT(".deadbeef.tmp");
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(StaleRecoveryTarget), true);
    TestTrue(TEXT("Stale recovery temporary fixture writes"),
        FFileHelper::SaveStringToFile(
            TEXT("encrypted-credential-fragment"),
            *StaleRecoveryTemporary,
            FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
    TestFalse(TEXT("Stale recovery temporary blocks backup creation"),
        RecoverySource.ExportRecoveryBundle(
            StaleRecoveryTarget, RecoveryPassphrase, &Error));
    TestFalse(TEXT("Blocked recovery export creates no final bundle"),
        IFileManager::Get().FileExists(*StaleRecoveryTarget));
    IFileManager::Get().Delete(*StaleRecoveryTemporary, false, false, true);
    const FString RecoveryBundle = FPaths::Combine(Root, TEXT("Recovery/source.akugcrecovery"));
    TestTrue(TEXT("Password-encrypted recovery bundle exports"),
        RecoverySource.ExportRecoveryBundle(RecoveryBundle, RecoveryPassphrase, &Error));
    FString RecoveryJson;
    TestTrue(TEXT("Recovery bundle is readable as bounded JSON"),
        FFileHelper::LoadFileToString(RecoveryJson, *RecoveryBundle));
    TestFalse(TEXT("Recovery bundle does not contain a plaintext private-key field"),
        RecoveryJson.Contains(TEXT("\"privateKey\"")));
    TestTrue(TEXT("Recovery bundle declares scrypt and AES-GCM"),
        RecoveryJson.Contains(TEXT("\"protection\":\"scrypt-aes-256-gcm-v1\"")));

    const FString WrongRestoreDirectory = FPaths::Combine(Root, TEXT("WrongRestore"));
    FAkUGCPublisherIdentityInfo WrongRestoreInfo;
    TestFalse(TEXT("Wrong recovery password is rejected"),
        FAkUGCPublisherKeyStore(WrongRestoreDirectory).ImportRecoveryBundle(
            RecoveryBundle, WrongPassphrase, WrongRestoreInfo, &Error));
    TestFalse(TEXT("Wrong password creates no local identity"),
        IFileManager::Get().FileExists(*FPaths::Combine(
            WrongRestoreDirectory, TEXT("publisher-identity.json"))));

    FString TamperedRecoveryJson = RecoveryJson;
    const FString RecoveryCiphertextField = TEXT("\"ciphertext\":\"");
    const int32 RecoveryCiphertextIndex =
        TamperedRecoveryJson.Find(RecoveryCiphertextField) + RecoveryCiphertextField.Len();
    TestTrue(TEXT("Recovery bundle contains ciphertext"),
        RecoveryCiphertextIndex >= RecoveryCiphertextField.Len());
    if (RecoveryCiphertextIndex >= RecoveryCiphertextField.Len()
        && RecoveryCiphertextIndex < TamperedRecoveryJson.Len())
    {
        TamperedRecoveryJson[RecoveryCiphertextIndex] =
            TamperedRecoveryJson[RecoveryCiphertextIndex] == TEXT('A') ? TEXT('B') : TEXT('A');
    }
    const FString TamperedRecoveryBundle =
        FPaths::Combine(Root, TEXT("Recovery/tampered.akugcrecovery"));
    TestTrue(TEXT("Tampered recovery fixture writes"),
        FFileHelper::SaveStringToFile(
            TamperedRecoveryJson,
            *TamperedRecoveryBundle,
            FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
    const FString TamperedRestoreDirectory = FPaths::Combine(Root, TEXT("TamperedRestore"));
    TestFalse(TEXT("Tampered recovery ciphertext is rejected"),
        FAkUGCPublisherKeyStore(TamperedRestoreDirectory).ImportRecoveryBundle(
            TamperedRecoveryBundle, RecoveryPassphrase, WrongRestoreInfo, &Error));
    TestFalse(TEXT("Tampered recovery creates no local identity"),
        IFileManager::Get().FileExists(*FPaths::Combine(
            TamperedRestoreDirectory, TEXT("publisher-identity.json"))));

    const FString RestoredDirectory = FPaths::Combine(Root, TEXT("RestoredPublisher"));
    const FAkUGCPublisherKeyStore RestoredPublisher(RestoredDirectory);
    FAkUGCPublisherIdentityInfo RestoredInfo;
    TestTrue(TEXT("Recovery bundle restores into an empty identity directory"),
        RestoredPublisher.ImportRecoveryBundle(
            RecoveryBundle, RecoveryPassphrase, RestoredInfo, &Error));
    TestEqual(TEXT("Restored identity preserves public key"),
        RestoredInfo.PublicKey, RecoverySourceInfo.PublicKey);
    TestEqual(TEXT("Restored identity preserves key version"),
        RestoredInfo.KeyVersion, RecoverySourceInfo.KeyVersion);
    TestTrue(TEXT("Repeated import of the same recovery bundle is idempotent"),
        RestoredPublisher.ImportRecoveryBundle(
            RecoveryBundle, RecoveryPassphrase, RestoredInfo, &Error));
    FString UndecryptableIdentity;
    TestTrue(TEXT("Restored DPAPI identity fixture is readable"),
        FFileHelper::LoadFileToString(
            UndecryptableIdentity,
            *RestoredPublisher.IdentityPath()));
    const FString ProtectedPrivateKeyField = TEXT("\"protectedPrivateKey\":\"");
    const int32 ProtectedPrivateKeyIndex =
        UndecryptableIdentity.Find(ProtectedPrivateKeyField)
        + ProtectedPrivateKeyField.Len();
    TestTrue(TEXT("Restored identity contains DPAPI ciphertext"),
        ProtectedPrivateKeyIndex >= ProtectedPrivateKeyField.Len());
    if (ProtectedPrivateKeyIndex >= ProtectedPrivateKeyField.Len()
        && ProtectedPrivateKeyIndex < UndecryptableIdentity.Len())
    {
        UndecryptableIdentity[ProtectedPrivateKeyIndex] =
            UndecryptableIdentity[ProtectedPrivateKeyIndex] == TEXT('A')
                ? TEXT('B')
                : TEXT('A');
    }
    TestTrue(TEXT("Undecryptable DPAPI fixture writes"),
        FFileHelper::SaveStringToFile(
            UndecryptableIdentity,
            *RestoredPublisher.IdentityPath(),
            FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
    FString UnavailablePublicKey;
    TestFalse(TEXT("Tampered DPAPI identity cannot load normally"),
        RestoredPublisher.GetOrCreatePublicKey(UnavailablePublicKey, &Error));
    TestTrue(TEXT("Matching recovery bundle can rewrap an undecryptable DPAPI identity"),
        RestoredPublisher.ImportRecoveryBundle(
            RecoveryBundle, RecoveryPassphrase, RestoredInfo, &Error));
    TestTrue(TEXT("Rewrapped DPAPI identity loads normally"),
        RestoredPublisher.GetOrCreatePublicKey(UnavailablePublicKey, &Error));
    TestEqual(TEXT("Rewrapped identity preserves public key"),
        UnavailablePublicKey, RecoverySourceInfo.PublicKey);

    FAkUGCLogicPackSignature SourceSignature, RestoredSignature;
    const FAkUGCLogicPackManifest RecoveryFixtureManifest = Loaded.Pack.Manifest;
    TestTrue(TEXT("Recovery source signs fixture"),
        RecoverySource.Sign(RecoveryFixtureManifest, SourceSignature, &Error));
    TestTrue(TEXT("Restored identity signs fixture"),
        RestoredPublisher.Sign(RecoveryFixtureManifest, RestoredSignature, &Error));
    TestEqual(TEXT("Restored Ed25519 identity produces the same deterministic signature"),
        RestoredSignature.Signature, SourceSignature.Signature);

    const FString ConflictDirectory = FPaths::Combine(Root, TEXT("ConflictPublisher"));
    const FAkUGCPublisherKeyStore ConflictPublisher(ConflictDirectory);
    FString ConflictPublicKeyBefore, ConflictPublicKeyAfter;
    TestTrue(TEXT("Conflicting identity fixture creates"),
        ConflictPublisher.GetOrCreatePublicKey(ConflictPublicKeyBefore, &Error));
    TestFalse(TEXT("Recovery cannot overwrite another identity"),
        ConflictPublisher.ImportRecoveryBundle(
            RecoveryBundle, RecoveryPassphrase, WrongRestoreInfo, &Error));
    TestTrue(TEXT("Conflicting identity remains readable after rejection"),
        ConflictPublisher.GetOrCreatePublicKey(ConflictPublicKeyAfter, &Error));
    TestEqual(TEXT("Rejected recovery does not rotate conflicting identity"),
        ConflictPublicKeyAfter, ConflictPublicKeyBefore);

    const FString RotatedRecoveryBundle =
        FPaths::Combine(Root, TEXT("Recovery/rotated.akugcrecovery"));
    FAkUGCPublisherIdentityInfo RotatedInfo;
    TestTrue(TEXT("Explicit rotation creates a new identity and immediate recovery bundle"),
        RestoredPublisher.RotateWithRecovery(
            RotatedRecoveryBundle, RecoveryPassphrase, RotatedInfo, &Error));
    TestEqual(TEXT("Rotation increments key version"), RotatedInfo.KeyVersion, 2);
    TestEqual(TEXT("Rotation appends one history record"), RotatedInfo.Rotations.Num(), 1);
    TestNotEqual(TEXT("Rotation changes current public key"),
        RotatedInfo.PublicKey, RecoverySourceInfo.PublicKey);
    TestTrue(TEXT("Rotation history has valid old/new signatures"),
        FAkUGCPublisherKeyStore::VerifyIdentityHistory(RotatedInfo, &Error));
    TestTrue(TEXT("Rotation recovery bundle exists"),
        IFileManager::Get().FileExists(*RotatedRecoveryBundle));
    FString RotatedPublicKey;
    TestTrue(TEXT("Rotated identity reloads"),
        RestoredPublisher.GetOrCreatePublicKey(RotatedPublicKey, &Error));
    TestEqual(TEXT("Reloaded identity uses rotated public key"),
        RotatedPublicKey, RotatedInfo.PublicKey);
    TestTrue(TEXT("Pre-rotation signature remains verifiable by historical key"),
        FAkUGCLogicPackVerifier::Verify(
            RecoveryFixtureManifest,
            SourceSignature,
            RecoverySourceInfo.PublicKey,
            &Error));
    TestFalse(TEXT("Old recovery bundle cannot roll a rotated store back"),
        RestoredPublisher.ImportRecoveryBundle(
            RecoveryBundle, RecoveryPassphrase, WrongRestoreInfo, &Error));

    const FString RotatedRestoreDirectory = FPaths::Combine(Root, TEXT("RotatedRestore"));
    FAkUGCPublisherIdentityInfo RotatedRestoreInfo;
    TestTrue(TEXT("Rotated recovery bundle restores full history"),
        FAkUGCPublisherKeyStore(RotatedRestoreDirectory).ImportRecoveryBundle(
            RotatedRecoveryBundle, RecoveryPassphrase, RotatedRestoreInfo, &Error));
    TestEqual(TEXT("Restored rotation keeps current public key"),
        RotatedRestoreInfo.PublicKey, RotatedInfo.PublicKey);
    TestEqual(TEXT("Restored rotation keeps key version"),
        RotatedRestoreInfo.KeyVersion, RotatedInfo.KeyVersion);
    TestTrue(TEXT("Restored rotation history verifies"),
        FAkUGCPublisherKeyStore::VerifyIdentityHistory(RotatedRestoreInfo, &Error));

    const FString ReusedRotationPath = RotatedRecoveryBundle;
    const FString PublicKeyBeforeRejectedRotation = RotatedPublicKey;
    TestFalse(TEXT("Rotation refuses to overwrite an existing recovery bundle"),
        RestoredPublisher.RotateWithRecovery(
            ReusedRotationPath, RecoveryPassphrase, WrongRestoreInfo, &Error));
    TestTrue(TEXT("Identity reloads after rejected rotation"),
        RestoredPublisher.GetOrCreatePublicKey(RotatedPublicKey, &Error));
    TestEqual(TEXT("Rejected rotation leaves current key unchanged"),
        RotatedPublicKey, PublicKeyBeforeRejectedRotation);

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

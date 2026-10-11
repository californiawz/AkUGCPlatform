#include "Misc/AutomationTest.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Pack/AkUGCWorkLibrary.h"
#include "Pack/AkUGCLogicPackCodec.h"
#include "Pack/AkUGCLogicPackHasher.h"
#include "Pack/AkUGCLogicPackSignature.h"
#include "Prefab/AkUGCOfficialPrefabCatalog.h"
#include "Prefab/AkUGCPrefabRegistry.h"
#include "Template/AkUGCTowerDefenseTemplate.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAkUGCWorkLibraryTest, "AkUGC.Core.WorkLibrary.PublishImportAndIsolation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ServerContext | EAutomationTestFlags::EngineFilter)
bool FAkUGCWorkLibraryTest::RunTest(const FString& Parameters)
{
    const FString Root = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation/WorkLibrary"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
    FAkUGCWorkLibrary Author(FPaths::Combine(Root, TEXT("Author"))), Receiver(FPaths::Combine(Root, TEXT("Receiver")));
    FString PrivateKey, ReusedKey, Error, Share;
    const FString KeyPath = FPaths::Combine(Root, TEXT("Private/private.key"));
    TestTrue(TEXT("Publisher key generated locally"), FAkUGCWorkLibrary::GetOrCreatePublisherKey(KeyPath, PrivateKey, &Error));
    TestTrue(TEXT("Publisher identity persists"), FAkUGCWorkLibrary::GetOrCreatePublisherKey(KeyPath, ReusedKey, &Error));
    TestEqual(TEXT("Key reused, not rotated silently"), PrivateKey, ReusedKey);
    FAkUGCPrefabRegistry Registry;
    FAkUGCProjectDocument Document;
    FAkUGCOfficialPrefabCatalog::RegisterTowerDefense(Registry);
    FAkUGCTowerDefenseTemplate::CreateDefaultDocument(Registry, Document);
    Document.Manifest.DisplayName = TEXT("独立分享作品");
    const FString SourceHash = FAkUGCLogicPackHasher::HashDocument(Document);
    FAkUGCWorkEntry Published, Imported;
    if (!Author.Publish(Document, PrivateKey, FPaths::Combine(Root, TEXT("Shares")), Published, Share, &Error))
    { AddError(Error); IFileManager::Get().DeleteDirectory(*Root, false, true); return false; }
    FString ShareJson;
    FFileHelper::LoadFileToString(ShareJson, *Share);
    TestFalse(TEXT("Share does not contain private key"), ShareJson.Contains(PrivateKey));
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
    TestFalse(TEXT("Unwritable share path fails publish"), Author.Publish(Document, PrivateKey, Blocked, Failed, FailedShare, &Error));
    Author.List(Entries, Rejected);
    TestEqual(TEXT("Failed publish preserves original library only"), Entries.Num(), 1);
    FAkUGCWorkLibrary BlockedLibrary(Blocked);
    TestFalse(TEXT("Library commit failure fails publish"), BlockedLibrary.Publish(Document, PrivateKey, FPaths::Combine(Root, TEXT("Rollback")), Failed, FailedShare, &Error));
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
    FAkUGCLogicPackSigner::Sign(Mutated.Manifest, PrivateKey, Mutated.Signature);
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
    FFileHelper::SaveStringToFile(TEXT("invalid key"), *KeyPath);
    TestFalse(TEXT("Corrupt publisher key is not auto-replaced"), FAkUGCWorkLibrary::GetOrCreatePublisherKey(KeyPath, ReusedKey, &Error));
    PrivateKey.Reset(); ReusedKey.Reset();
    IFileManager::Get().DeleteDirectory(*Root, false, true);
    return true;
}
#endif

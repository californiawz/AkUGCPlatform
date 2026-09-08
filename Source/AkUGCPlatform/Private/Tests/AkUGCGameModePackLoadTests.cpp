#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"

#include "AkUGCPlayableSceneFactory.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Game/AkUGCGameMode.h"
#include "Document/AkUGCDocument.h"
#include "Pack/AkUGCLogicPack.h"
#include "Pack/AkUGCLogicPackBuilder.h"
#include "Pack/AkUGCLogicPackCodec.h"
#include "Pack/AkUGCLogicPackLoader.h"
#include "Pack/AkUGCLogicPackSignature.h"
#include "Prefab/AkUGCOfficialPrefabCatalog.h"
#include "Prefab/AkUGCPrefabRegistry.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AkUGCGameModePackLoadTest
{
    struct FScopedTempFile
    {
        FString Path;

        explicit FScopedTempFile(const TCHAR* Extension)
        {
            Path = FPaths::CreateTempFilename(
                *FPaths::ProjectSavedDir(),
                TEXT("AkUGCGameModePack"),
                Extension);
        }

        ~FScopedTempFile()
        {
            if (!Path.IsEmpty())
            {
                IFileManager::Get().Delete(*Path, /*bRequireExists=*/false, /*bEvenReadOnly=*/true);
            }
        }
    };

    UWorld* CreateTestWorld()
    {
        UWorld* World = NewObject<UWorld>(GetTransientPackage(), NAME_None, RF_Transient);
        World->WorldType = EWorldType::Game;
        FWorldContext& WorldContext = GEngine->CreateNewWorldContext(World->WorldType);
        WorldContext.SetCurrentWorld(World);
        World->InitializeNewWorld(UWorld::InitializationValues()
            .InitializeScenes(false)
            .AllowAudioPlayback(false)
            .RequiresHitProxies(false)
            .CreatePhysicsScene(false)
            .CreateNavigation(false)
            .CreateAISystem(false)
            .ShouldSimulatePhysics(false)
            .EnableTraceCollision(false)
            .SetTransactional(false));
        return World;
    }

    void DestroyTestWorld(UWorld* World)
    {
        GEngine->DestroyWorldContext(World);
        World->DestroyWorld(false);
    }

    bool WriteSignedPackFile(
        const FAkUGCProjectDocument& Document,
        const FAkUGCLogicPackKeyPair& KeyPair,
        const FString& FilePath,
        FString* OutError)
    {
        const FAkUGCLogicPackBuildResult Build = FAkUGCLogicPackBuilder::Build(Document);
        if (!Build.bSucceeded)
        {
            if (OutError)
            {
                *OutError = Build.ErrorMessage;
            }
            return false;
        }

        FAkUGCLogicPack Pack = Build.Pack;
        if (!FAkUGCLogicPackSigner::Sign(Pack.Manifest, KeyPair.PrivateKey, Pack.Signature, OutError))
        {
            return false;
        }

        FString Json;
        if (!FAkUGCLogicPackCodec::Serialize(Pack, Json, OutError))
        {
            return false;
        }

        return FFileHelper::SaveStringToFile(Json, *FilePath);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCGameModePackLoadTest,
    "AkUGC.Runtime.GameMode.PackLoad",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCGameModePackLoadTest::RunTest(const FString& Parameters)
{
    if (!GEngine)
    {
        AddError(TEXT("Engine is not available."));
        return false;
    }

    FAkUGCProjectDocument Document;
    FString Error;
    TestTrue(*Error, AkUGCPlayableSceneFactory::MakePlayableTowerDefenseDocument(Document, &Error));

    FAkUGCLogicPackKeyPair KeyPair;
    TestTrue(*Error, FAkUGCLogicPackSigner::GenerateKeyPair(KeyPair, &Error));

    AkUGCGameModePackLoadTest::FScopedTempFile TempFile(TEXT(".json"));
    TestTrue(*Error, AkUGCGameModePackLoadTest::WriteSignedPackFile(Document, KeyPair, TempFile.Path, &Error));

    UWorld* World = AkUGCGameModePackLoadTest::CreateTestWorld();
    AAkUGCGameMode* GameMode = World->SpawnActor<AAkUGCGameMode>();
    TestNotNull(TEXT("GameMode spawns in test world"), GameMode);
    if (!GameMode)
    {
        AkUGCGameModePackLoadTest::DestroyTestWorld(World);
        return false;
    }

    TestFalse(TEXT("GameMode starts without authority session"), GameMode->HasAuthoritySession());

    // 签名 Pack 成功加载并初始化权威会话。
    TestTrue(*Error, GameMode->LoadAndInitializeAuthoritySession(TempFile.Path, KeyPair.PublicKey, &Error));
    TestTrue(TEXT("Authority session is initialized from signed pack"), GameMode->HasAuthoritySession());

    // 重复初始化被拒绝。
    const bool bSecond = GameMode->LoadAndInitializeAuthoritySession(TempFile.Path, KeyPair.PublicKey, &Error);
    TestFalse(TEXT("Duplicate initialization is rejected"), bSecond);
    TestTrue(TEXT("Duplicate rejection carries an error"), Error.Contains(TEXT("already initialized")));

    AkUGCGameModePackLoadTest::DestroyTestWorld(World);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCGameModePackRejectTest,
    "AkUGC.Runtime.GameMode.PackReject",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCGameModePackRejectTest::RunTest(const FString& Parameters)
{
    if (!GEngine)
    {
        AddError(TEXT("Engine is not available."));
        return false;
    }

    FAkUGCProjectDocument Document;
    FString Error;
    TestTrue(*Error, AkUGCPlayableSceneFactory::MakePlayableTowerDefenseDocument(Document, &Error));

    FAkUGCLogicPackKeyPair KeyPair;
    FAkUGCLogicPackKeyPair OtherKeyPair;
    TestTrue(*Error, FAkUGCLogicPackSigner::GenerateKeyPair(KeyPair, &Error));
    TestTrue(*Error, FAkUGCLogicPackSigner::GenerateKeyPair(OtherKeyPair, &Error));

    // 1. 未签名 Pack 文件被拒绝（验签加载要求签名）。
    const FAkUGCLogicPackBuildResult Build = FAkUGCLogicPackBuilder::Build(Document);
    TestTrue(*Build.ErrorMessage, Build.bSucceeded);
    FString UnsignedJson;
    TestTrue(*Error, FAkUGCLogicPackCodec::Serialize(Build.Pack, UnsignedJson, &Error));
    AkUGCGameModePackLoadTest::FScopedTempFile UnsignedFile(TEXT(".json"));
    TestTrue(TEXT("Unsigned pack is written"), FFileHelper::SaveStringToFile(UnsignedJson, *UnsignedFile.Path));

    UWorld* World = AkUGCGameModePackLoadTest::CreateTestWorld();
    AAkUGCGameMode* GameMode = World->SpawnActor<AAkUGCGameMode>();
    TestNotNull(TEXT("GameMode spawns in test world"), GameMode);
    if (!GameMode)
    {
        AkUGCGameModePackLoadTest::DestroyTestWorld(World);
        return false;
    }

    TestFalse(*Error, GameMode->LoadAndInitializeAuthoritySession(UnsignedFile.Path, KeyPair.PublicKey, &Error));
    TestFalse(TEXT("Unsigned pack does not initialize a session"), GameMode->HasAuthoritySession());

    // 2. 缺失文件被拒绝。
    TestFalse(*Error, GameMode->LoadAndInitializeAuthoritySession(
        TEXT("E:/__definitely_missing__.json"), KeyPair.PublicKey, &Error));
    TestFalse(TEXT("Missing file does not initialize a session"), GameMode->HasAuthoritySession());

    // 3. 错误公钥被拒绝。
    AkUGCGameModePackLoadTest::FScopedTempFile SignedFile(TEXT(".json"));
    TestTrue(*Error, AkUGCGameModePackLoadTest::WriteSignedPackFile(Document, KeyPair, SignedFile.Path, &Error));
    TestFalse(*Error, GameMode->LoadAndInitializeAuthoritySession(SignedFile.Path, OtherKeyPair.PublicKey, &Error));
    TestFalse(TEXT("Wrong trusted key does not initialize a session"), GameMode->HasAuthoritySession());

    // 4. 签名合法但内容非法（缺 base）的 Pack 在运行时被拒绝。
    FAkUGCProjectDocument InvalidDocument;
    TestTrue(*Error, AkUGCPlayableSceneFactory::MakePlayableTowerDefenseDocument(InvalidDocument, &Error));
    InvalidDocument.Scenes[0].Entities.RemoveAll([](const FAkUGCEntityRecord& Entity)
    {
        return Entity.PrefabId == TEXT("official.gameplay.base");
    });
    AkUGCGameModePackLoadTest::FScopedTempFile InvalidFile(TEXT(".json"));
    TestTrue(*Error, AkUGCGameModePackLoadTest::WriteSignedPackFile(InvalidDocument, KeyPair, InvalidFile.Path, &Error));
    TestFalse(*Error, GameMode->LoadAndInitializeAuthoritySession(InvalidFile.Path, KeyPair.PublicKey, &Error));
    TestFalse(TEXT("Content-invalid pack does not initialize a session"), GameMode->HasAuthoritySession());
    TestTrue(TEXT("Content-invalid rejection triggers tower defense validation"),
        Error.Contains(TEXT("Tower defense")) && Error.Contains(TEXT("base")));

    AkUGCGameModePackLoadTest::DestroyTestWorld(World);
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

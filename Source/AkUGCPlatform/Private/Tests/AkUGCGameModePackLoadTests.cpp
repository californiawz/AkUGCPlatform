#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"

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

    /** 构造可通过服务器权威运行时校验的完整塔防场景（enemy_spawn + 路径 + 基地 + 终点 + 三波）。 */
    bool MakePlayableTowerDefenseDocument(FAkUGCProjectDocument& OutDocument, FString* OutError)
    {
        FAkUGCPrefabRegistry Registry;
        if (!FAkUGCOfficialPrefabCatalog::RegisterTowerDefense(Registry, OutError))
        {
            return false;
        }

        OutDocument = FAkUGCProjectDocument();
        OutDocument.Manifest.SchemaVersion = AkUGCSchema::CurrentProjectDocumentVersion;
        OutDocument.Manifest.ProjectId = FGuid(0x11111111, 0x22222222, 0x33333333, 0x44444444);
        OutDocument.Manifest.DisplayName = TEXT("Server Pack Load Test");
        OutDocument.Manifest.TemplateId = TEXT("official.tower_defense");
        OutDocument.Manifest.Capabilities.Add(TEXT("logic"));
        OutDocument.Manifest.Capabilities.Add(TEXT("ruleset"));

        FAkUGCSceneDocument& Scene = OutDocument.Scenes.AddDefaulted_GetRef();
        Scene.SceneId = FGuid(0xAAAAAAAA, 0xBBBBBBBB, 0xCCCCCCCC, 0xDDDDDDDD);
        Scene.DisplayName = TEXT("Main");
        Scene.Ruleset.WaveIntervalSeconds = 5.0;

        const FGuid SpawnPointId = FGuid::NewGuid();
        FAkUGCEntityRecord SpawnPoint;
        if (!Registry.CreateEntityRecord(
            TEXT("official.gameplay.enemy_spawn"),
            SpawnPointId,
            FTransform::Identity,
            SpawnPoint,
            OutError))
        {
            return false;
        }
        if (FAkUGCComponentRecord* SpawnConfig = SpawnPoint.Components.FindByPredicate(
            [](const FAkUGCComponentRecord& Component) { return Component.TypeId == TEXT("tower_defense.spawn"); }))
        {
            SpawnConfig->Properties.FindChecked(TEXT("enemyCount")).IntegerValue = 3;
            SpawnConfig->Properties.FindChecked(TEXT("spawnInterval")).NumberValue = 0.25;
        }
        Scene.Entities.Add(SpawnPoint);

        for (int32 WaveIndex = 0; WaveIndex < 3; ++WaveIndex)
        {
            FAkUGCTowerDefenseWave& Wave = Scene.Ruleset.Waves.AddDefaulted_GetRef();
            Wave.WaveId = FGuid(10 + WaveIndex, 0, 0, 0);
            Wave.SpawnPointEntityId = SpawnPointId;
            Wave.StartDelaySeconds = WaveIndex;
        }

        FAkUGCEntityRecord FirstPathNode;
        FAkUGCEntityRecord SecondPathNode;
        if (!Registry.CreateEntityRecord(
            TEXT("official.gameplay.path_node"),
            FGuid::NewGuid(),
            FTransform(FVector(500.0, 50.0, 0.0)),
            FirstPathNode,
            OutError)
            || !Registry.CreateEntityRecord(
            TEXT("official.gameplay.path_node"),
            FGuid::NewGuid(),
            FTransform(FVector(1000.0, 50.0, 0.0)),
            SecondPathNode,
            OutError))
        {
            return false;
        }
        FirstPathNode.Components[0].Properties.FindChecked(TEXT("order")).IntegerValue = 0;
        SecondPathNode.Components[0].Properties.FindChecked(TEXT("order")).IntegerValue = 1;
        Scene.Entities.Add(FirstPathNode);
        Scene.Entities.Add(SecondPathNode);

        FAkUGCEntityRecord BaseEntity;
        FAkUGCEntityRecord GoalEntity;
        if (!Registry.CreateEntityRecord(
            TEXT("official.gameplay.base"),
            FGuid::NewGuid(),
            FTransform(FVector(1100.0, 50.0, 0.0)),
            BaseEntity,
            OutError)
            || !Registry.CreateEntityRecord(
            TEXT("official.gameplay.goal"),
            FGuid::NewGuid(),
            FTransform(FVector(1000.0, 50.0, 0.0)),
            GoalEntity,
            OutError))
        {
            return false;
        }
        BaseEntity.Components[0].Properties.FindChecked(TEXT("maxHealth")).NumberValue = 25.0;
        GoalEntity.Components[0].Properties.FindChecked(TEXT("baseDamage")).NumberValue = 99.0;
        Scene.Entities.Add(BaseEntity);
        Scene.Entities.Add(GoalEntity);

        FAkUGCLogicNode& Start = Scene.LogicGraph.Nodes.AddDefaulted_GetRef();
        Start.NodeId = FGuid(2, 0, 0, 0);
        Start.Type = EAkUGCLogicNodeType::GameStart;

        FAkUGCLogicNode& Message = Scene.LogicGraph.Nodes.AddDefaulted_GetRef();
        Message.NodeId = FGuid(3, 0, 0, 0);
        Message.Type = EAkUGCLogicNodeType::Message;
        Message.Message = TEXT("Wave ready");

        FAkUGCLogicConnection& Connection = Scene.LogicGraph.Connections.AddDefaulted_GetRef();
        Connection.SourceNodeId = Start.NodeId;
        Connection.TargetNodeId = Message.NodeId;

        return true;
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
    TestTrue(*Error, AkUGCGameModePackLoadTest::MakePlayableTowerDefenseDocument(Document, &Error));

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
    TestTrue(*Error, AkUGCGameModePackLoadTest::MakePlayableTowerDefenseDocument(Document, &Error));

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
    TestTrue(*Error, AkUGCGameModePackLoadTest::MakePlayableTowerDefenseDocument(InvalidDocument, &Error));
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

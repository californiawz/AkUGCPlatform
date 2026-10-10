#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

#include "Command/AkUGCCommandExecutor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Game/AkUGCGameMode.h"
#include "Game/AkUGCGameState.h"
#include "Gameplay/AkUGCTowerDefenseStateHasher.h"
#include "Pack/AkUGCLogicPackBuilder.h"
#include "Pack/AkUGCLogicPackCodec.h"
#include "Pack/AkUGCLogicPackSignature.h"
#include "Prefab/AkUGCOfficialPrefabCatalog.h"
#include "Prefab/AkUGCPrefabRegistry.h"
#include "Subsystem/AkUGCLogicRuntimeSubsystem.h"
#include "Template/AkUGCTowerDefenseTemplate.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AkUGCTemplateAuthorityTest
{
    struct FScopedWorld
    {
        UWorld* World = nullptr;

        FScopedWorld()
        {
            World = NewObject<UWorld>(GetTransientPackage(), NAME_None, RF_Transient);
            World->WorldType = EWorldType::Game;
            GEngine->CreateNewWorldContext(World->WorldType).SetCurrentWorld(World);
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
        }

        ~FScopedWorld()
        {
            GEngine->DestroyWorldContext(World);
            World->DestroyWorld(false);
        }
    };

    struct FScopedPackFile
    {
        FString Path = FPaths::CreateTempFilename(*FPaths::ProjectSavedDir(), TEXT("AkUGCL1"), TEXT(".json"));

        ~FScopedPackFile()
        {
            IFileManager::Get().Delete(*Path, false, true);
        }
    };

    void StabilizeIds(FAkUGCProjectDocument& Document)
    {
        Document.Manifest.ProjectId = FGuid(0x11000001, 0, 0, 0);
        FAkUGCSceneDocument& Scene = Document.Scenes[0];
        Scene.SceneId = FGuid(0x11000002, 0, 0, 0);
        TMap<FGuid, FGuid> EntityIds;
        for (int32 Index = 0; Index < Scene.Entities.Num(); ++Index)
        {
            EntityIds.Add(Scene.Entities[Index].EntityId, FGuid(0x11000100 + Index, 0, 0, 0));
        }
        for (FAkUGCEntityRecord& Entity : Scene.Entities)
        {
            Entity.EntityId = EntityIds.FindChecked(Entity.EntityId);
            if (Entity.ParentEntityId.IsValid())
            {
                Entity.ParentEntityId = EntityIds.FindChecked(Entity.ParentEntityId);
            }
        }
        for (int32 Index = 0; Index < Scene.Ruleset.Waves.Num(); ++Index)
        {
            FAkUGCTowerDefenseWave& Wave = Scene.Ruleset.Waves[Index];
            Wave.WaveId = FGuid(0x11000200 + Index, 0, 0, 0);
            Wave.SpawnPointEntityId = EntityIds.FindChecked(Wave.SpawnPointEntityId);
        }
        TMap<FGuid, FGuid> NodeIds;
        for (int32 Index = 0; Index < Scene.LogicGraph.Nodes.Num(); ++Index)
        {
            NodeIds.Add(Scene.LogicGraph.Nodes[Index].NodeId, FGuid(0x11000300 + Index, 0, 0, 0));
        }
        for (FAkUGCLogicNode& Node : Scene.LogicGraph.Nodes)
        {
            Node.NodeId = NodeIds.FindChecked(Node.NodeId);
            if (Node.SpawnAtEntityId.IsValid())
            {
                Node.SpawnAtEntityId = EntityIds.FindChecked(Node.SpawnAtEntityId);
            }
        }
        for (FAkUGCLogicConnection& Connection : Scene.LogicGraph.Connections)
        {
            Connection.SourceNodeId = NodeIds.FindChecked(Connection.SourceNodeId);
            Connection.TargetNodeId = NodeIds.FindChecked(Connection.TargetNodeId);
        }
    }

    bool RunSignedDocument(
        const FAkUGCProjectDocument& Document,
        const FAkUGCLogicPackKeyPair& Keys,
        bool bSmallDelta,
        FAkUGCTowerDefenseFinalState& OutState,
        int32& OutResources,
        FString& OutError)
    {
        const FAkUGCLogicPackBuildResult Build = FAkUGCLogicPackBuilder::Build(Document);
        if (!Build.bSucceeded)
        {
            OutError = Build.ErrorMessage;
            return false;
        }
        FAkUGCLogicPack Pack = Build.Pack;
        FString Json;
        FScopedPackFile File;
        if (!FAkUGCLogicPackSigner::Sign(Pack.Manifest, Keys.PrivateKey, Pack.Signature, &OutError)
            || !FAkUGCLogicPackCodec::Serialize(Pack, Json, &OutError)
            || !FFileHelper::SaveStringToFile(Json, *File.Path))
        {
            return false;
        }

        FScopedWorld ScopedWorld;
        AAkUGCGameMode* Mode = ScopedWorld.World->SpawnActor<AAkUGCGameMode>();
        AAkUGCGameState* State = ScopedWorld.World->SpawnActor<AAkUGCGameState>();
        if (!Mode || !State || !Mode->LoadAndInitializeAuthoritySession(File.Path, Keys.PublicKey, &OutError))
        {
            return false;
        }
        UAkUGCLogicRuntimeSubsystem* Logic = ScopedWorld.World->GetSubsystem<UAkUGCLogicRuntimeSubsystem>();
        if (!Logic)
        {
            OutError = TEXT("Logic subsystem unavailable.");
            return false;
        }
        const int32 Steps = bSmallDelta ? 480 : 1;
        const double Delta = bSmallDelta ? 0.25 : 120.0;
        for (int32 Step = 0; Step < Steps; ++Step)
        {
            const FAkUGCLogicRuntimeResult Result = Logic->AdvanceLogicTime(Delta);
            if (!Result.bSucceeded)
            {
                OutError = Result.ErrorMessage;
                return false;
            }
        }
        Mode->ProjectStateToGameState(State);
        const FAkUGCWaveRuntimeSnapshot Wave = State->GetWaveSnapshot();
        OutState.WaveState = Wave.State;
        OutState.MatchResult = Wave.Result;
        OutState.CurrentWaveIndex = Wave.CurrentWaveIndex;
        OutState.CurrentWaveId = Wave.CurrentWaveId;
        OutState.TotalWaveCount = Wave.TotalWaveCount;
        OutState.BaseCurrentHealth = State->GetBaseHealthCurrent();
        OutState.BaseMaximumHealth = State->GetBaseHealthMaximum();
        OutState.ActiveEnemyCount = State->GetActiveEnemyCount();
        OutState.Messages = Logic->GetEmittedMessages();
        OutState.Spawns = Logic->GetSpawnedEntities();
        OutState.Goals = Logic->GetGoalReachedEntities();
        OutState.Damages = Logic->GetDamageEvents();
        OutState.Deaths = Logic->GetDeathEvents();
        OutResources = State->GetResources();
        return true;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCTemplateAuthorityConsistencyTest,
    "AkUGC.Runtime.GameMode.TowerDefenseTemplateConsistency",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
        | EAutomationTestFlags::ServerContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCTemplateAuthorityConsistencyTest::RunTest(const FString& Parameters)
{
    using namespace AkUGCTemplateAuthorityTest;
    FAkUGCPrefabRegistry Registry;
    FAkUGCLogicPackKeyPair Keys;
    FString Error;
    if (!FAkUGCOfficialPrefabCatalog::RegisterTowerDefense(Registry, &Error)
        || !FAkUGCLogicPackSigner::GenerateKeyPair(Keys, &Error))
    {
        AddError(Error);
        return false;
    }

    FAkUGCProjectDocument DefaultDocument;
    FAkUGCTowerDefenseFinalState DefaultState;
    int32 DefaultResources = 0;
    if (!FAkUGCTowerDefenseTemplate::CreateDefaultDocument(Registry, DefaultDocument, &Error)
        || !RunSignedDocument(DefaultDocument, Keys, false, DefaultState, DefaultResources, Error))
    {
        AddError(Error);
        return false;
    }
    TestTrue(TEXT("Unedited default template completes a match"), DefaultState.MatchResult != EAkUGCTowerDefenseMatchResult::InProgress);
    TestEqual(TEXT("Default template initializes 200 resources"), DefaultResources, 200);

    for (int32 WaveCount = 1; WaveCount <= 3; ++WaveCount)
    {
        FAkUGCProjectDocument Document;
        if (!FAkUGCTowerDefenseTemplate::CreateDefaultDocument(Registry, Document, &Error))
        {
            AddError(Error);
            return false;
        }
        FAkUGCTowerDefenseParameters Draft;
        Draft.Waves.SetNum(WaveCount);
        Draft.InitialResources = 700 + WaveCount;
        Draft.BaseMaxHealth = 100000.0;
        Draft.WaveIntervalSeconds = 0.1;
        int32 ExpectedEnemies = 0;
        double ExpectedDamage = 0.0;
        for (int32 Index = 0; Index < WaveCount; ++Index)
        {
            FAkUGCTowerDefenseWaveParameters& Wave = Draft.Waves[Index];
            Wave.EnemyCount = Index + 1;
            Wave.SpawnIntervalSeconds = 0.1;
            Wave.StartDelaySeconds = 0.1;
            Wave.EnemyMaxHealth = 50.0 + Index * 25.0;
            Wave.EnemyMoveSpeed = 400.0 + Index * 100.0;
            Wave.EnemyGoalDamage = 7.0 + Index;
            ExpectedEnemies += Wave.EnemyCount;
            ExpectedDamage += Wave.EnemyCount * Wave.EnemyGoalDamage;
        }
        FAkUGCCommandTransaction Transaction;
        if (!FAkUGCTowerDefenseTemplate::BuildTransaction(
                Document, Document.Scenes[0].SceneId, Draft, Registry, Transaction, &Error)
            || !FAkUGCCommandExecutor::Apply(Document, Transaction).bSucceeded)
        {
            AddError(Error);
            return false;
        }
        Document.Scenes[0].Entities.RemoveAll([](const FAkUGCEntityRecord& Entity)
        {
            return Entity.PrefabId == TEXT("official.tower.basic");
        });
        StabilizeIds(Document);
        FAkUGCTowerDefenseFinalState LargeState;
        FAkUGCTowerDefenseFinalState SmallState;
        int32 LargeResources = 0;
        int32 SmallResources = 0;
        if (!RunSignedDocument(Document, Keys, false, LargeState, LargeResources, Error)
            || !RunSignedDocument(Document, Keys, true, SmallState, SmallResources, Error))
        {
            AddError(FString::Printf(TEXT("%d-wave template failed: %s"), WaveCount, *Error));
            return false;
        }
        TestEqual(TEXT("Configured wave count reaches authority"), LargeState.TotalWaveCount, WaveCount);
        TestEqual(TEXT("All configured enemies spawn"), LargeState.Spawns.Num(), ExpectedEnemies);
        TestEqual(TEXT("Configured speed reaches goal within bounded time"), LargeState.Goals.Num(), ExpectedEnemies);
        TestEqual(TEXT("Configured base maximum reaches authority"), LargeState.BaseMaximumHealth, Draft.BaseMaxHealth);
        TestEqual(TEXT("Per-wave goal damage is consumed"), LargeState.BaseCurrentHealth, Draft.BaseMaxHealth - ExpectedDamage);
        TestEqual(TEXT("Authority initializes configured resources"), LargeResources, Draft.InitialResources);
        TestEqual(TEXT("Resource initialization is step independent"), SmallResources, LargeResources);
        TestEqual(TEXT("No enemies remain"), LargeState.ActiveEnemyCount, 0);
        TestTrue(TEXT("All waves finish"), LargeState.MatchResult == EAkUGCTowerDefenseMatchResult::Victory);
        const FString Hash = FAkUGCTowerDefenseStateHasher::HashFinalState(LargeState);
        TestEqual(TEXT("Template final hash is step independent"),
            FAkUGCTowerDefenseStateHasher::HashFinalState(SmallState), Hash);
        UE_LOG(LogTemp, Display, TEXT("[TowerDefenseTemplateConsistency] Waves=%d Resources=%d Hash=%s"),
            WaveCount, LargeResources, *Hash);
    }
    return true;
}

#endif

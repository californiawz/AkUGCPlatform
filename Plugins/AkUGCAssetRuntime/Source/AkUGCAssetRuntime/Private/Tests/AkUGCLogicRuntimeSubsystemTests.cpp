#include "Misc/AutomationTest.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Entity/AkUGCEntityBindingComponent.h"
#include "Prefab/AkUGCOfficialPrefabCatalog.h"
#include "Prefab/AkUGCPrefabRegistry.h"
#include "Scene/AkUGCSceneRuntime.h"
#include "Session/AkUGCDocumentRuntimeSession.h"
#include "Subsystem/AkUGCLogicRuntimeSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCLogicRuntimeSubsystemTest,
    "AkUGC.Runtime.Logic.GameStartMessage",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCLogicRuntimeSubsystemTest::RunTest(const FString& Parameters)
{
    if (!GEngine)
    {
        AddError(TEXT("Engine is not available."));
        return false;
    }

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

    UAkUGCLogicRuntimeSubsystem* Subsystem = World->GetSubsystem<UAkUGCLogicRuntimeSubsystem>();
    TestNotNull(TEXT("Logic Runtime world subsystem exists"), Subsystem);
    if (!Subsystem)
    {
        GEngine->DestroyWorldContext(World);
        World->DestroyWorld(false);
        return false;
    }

    FAkUGCLogicNode Start;
    Start.NodeId = FGuid::NewGuid();
    Start.Type = EAkUGCLogicNodeType::GameStart;
    FAkUGCLogicNode Message;
    Message.NodeId = FGuid::NewGuid();
    Message.Type = EAkUGCLogicNodeType::Message;
    Message.Message = TEXT("Runtime ready");
    FAkUGCLogicConnection Connection;
    Connection.SourceNodeId = Start.NodeId;
    Connection.TargetNodeId = Message.NodeId;
    FAkUGCLogicGraph Graph;
    Graph.Nodes = {Message, Start};
    Graph.Connections.Add(Connection);

    const FAkUGCLogicRuntimeResult RunResult = Subsystem->RunGameStart(Graph);
    TestTrue(TEXT("World subsystem runs Game Start graph"), RunResult.bSucceeded);
    TestEqual(TEXT("World subsystem executes two instructions"), RunResult.ExecutedInstructionCount, 2);
    TestEqual(TEXT("World subsystem records one message"), Subsystem->GetEmittedMessages().Num(), 1);
    if (Subsystem->GetEmittedMessages().Num() == 1)
    {
        TestEqual(TEXT("Runtime message text is observable"),
            Subsystem->GetEmittedMessages()[0].Message,
            FString(TEXT("Runtime ready")));
        TestEqual(TEXT("Runtime message source is observable"),
            Subsystem->GetEmittedMessages()[0].SourceNodeId,
            Message.NodeId);
    }

    Subsystem->ResetLogicRuntime();
    TestTrue(TEXT("Reset clears emitted messages"), Subsystem->GetEmittedMessages().IsEmpty());

    FAkUGCProjectDocument Document;
    Document.Manifest.ProjectId = FGuid::NewGuid();
    Document.Manifest.DisplayName = TEXT("Logic Runtime Integration");
    Document.Manifest.TemplateId = TEXT("official.tower_defense");
    FAkUGCSceneDocument& Scene = Document.Scenes.AddDefaulted_GetRef();
    Scene.SceneId = FGuid::NewGuid();
    Scene.DisplayName = TEXT("Main");
    Scene.LogicGraph = Graph;
    FAkUGCPrefabRegistry Registry;
    {
        FAkUGCSceneRuntime SceneRuntime(World);
        FAkUGCDocumentRuntimeSession Session(SceneRuntime, Registry, Scene.SceneId);
        TestTrue(TEXT("Scene session initialization runs Game Start"), Session.Initialize(Document).bSucceeded);
        TestEqual(TEXT("Automatic Game Start emits the scene message"), Subsystem->GetEmittedMessages().Num(), 1);
    }
    TestTrue(TEXT("Scene unload clears Logic Runtime messages"), Subsystem->GetEmittedMessages().IsEmpty());

    Graph.Nodes[0].Message.Reset();
    TestFalse(TEXT("World subsystem rejects invalid graph"), Subsystem->RunGameStart(Graph).bSucceeded);
    TestTrue(TEXT("Failed run does not retain previous messages"), Subsystem->GetEmittedMessages().IsEmpty());

    GEngine->DestroyWorldContext(World);
    World->DestroyWorld(false);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCLogicRuntimeTimerSpawnTest,
    "AkUGC.Runtime.Logic.TimerSpawnsBasicEnemy",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCLogicRuntimeTimerSpawnTest::RunTest(const FString& Parameters)
{
    if (!GEngine)
    {
        AddError(TEXT("Engine is not available."));
        return false;
    }

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

    UAkUGCLogicRuntimeSubsystem* Subsystem = World->GetSubsystem<UAkUGCLogicRuntimeSubsystem>();
    TestNotNull(TEXT("Logic Runtime subsystem exists for Timer Spawn"), Subsystem);
    if (!Subsystem)
    {
        GEngine->DestroyWorldContext(World);
        World->DestroyWorld(false);
        return false;
    }

    FAkUGCPrefabRegistry Registry;
    FString Error;
    TestTrue(TEXT("Official prefab catalog registers"),
        FAkUGCOfficialPrefabCatalog::RegisterTowerDefense(Registry, &Error));

    FAkUGCProjectDocument Document;
    Document.Manifest.ProjectId = FGuid::NewGuid();
    Document.Manifest.DisplayName = TEXT("Timer Spawn Integration");
    Document.Manifest.TemplateId = TEXT("official.tower_defense");
    FAkUGCSceneDocument& Scene = Document.Scenes.AddDefaulted_GetRef();
    Scene.SceneId = FGuid::NewGuid();
    Scene.DisplayName = TEXT("Main");

    const FGuid SpawnPointId = FGuid::NewGuid();
    const FTransform SpawnPointTransform(FVector(250.0, 50.0, 0.0));
    FAkUGCEntityRecord SpawnPoint;
    TestTrue(TEXT("Enemy Spawn record is created"), Registry.CreateEntityRecord(
        TEXT("official.gameplay.enemy_spawn"),
        SpawnPointId,
        SpawnPointTransform,
        SpawnPoint,
        &Error));
    Scene.Entities.Add(SpawnPoint);

    FAkUGCLogicNode Start;
    Start.NodeId = FGuid::NewGuid();
    Start.Type = EAkUGCLogicNodeType::GameStart;
    FAkUGCLogicNode Timer;
    Timer.NodeId = FGuid::NewGuid();
    Timer.Type = EAkUGCLogicNodeType::Timer;
    Timer.DelaySeconds = 1.0;
    FAkUGCLogicNode Spawn;
    Spawn.NodeId = FGuid::NewGuid();
    Spawn.Type = EAkUGCLogicNodeType::Spawn;
    Spawn.SpawnPrefabId = TEXT("official.unit.basic_enemy");
    Spawn.SpawnAtEntityId = SpawnPointId;
    Scene.LogicGraph.Nodes = {Spawn, Timer, Start};

    FAkUGCLogicConnection StartToTimer;
    StartToTimer.SourceNodeId = Start.NodeId;
    StartToTimer.TargetNodeId = Timer.NodeId;
    FAkUGCLogicConnection TimerToSpawn;
    TimerToSpawn.SourceNodeId = Timer.NodeId;
    TimerToSpawn.TargetNodeId = Spawn.NodeId;
    Scene.LogicGraph.Connections = {TimerToSpawn, StartToTimer};

    {
        FAkUGCSceneRuntime Runtime(World);
        FAkUGCDocumentRuntimeSession Session(Runtime, Registry, Scene.SceneId);
        TestTrue(TEXT("Timer Spawn scene initializes"), Session.Initialize(Document).bSucceeded);
        TestEqual(TEXT("Only authored Spawn Point exists initially"), Runtime.Num(), 1);
        TestTrue(TEXT("Half delay advances successfully"), Subsystem->AdvanceLogicTime(0.5).bSucceeded);
        TestEqual(TEXT("Enemy is not spawned before Timer expires"), Runtime.Num(), 1);
        TestTrue(TEXT("Remaining delay advances successfully"), Subsystem->AdvanceLogicTime(0.5).bSucceeded);
        TestEqual(TEXT("Timer spawns one Basic Enemy"), Runtime.Num(), 2);
        TestEqual(TEXT("Logic Spawn does not modify authored Document"), Document.Scenes[0].Entities.Num(), 1);
        TestEqual(TEXT("Spawn result is observable"), Subsystem->GetSpawnedEntities().Num(), 1);
        if (Subsystem->GetSpawnedEntities().Num() == 1)
        {
            const FAkUGCLogicRuntimeSpawn RuntimeSpawn = Subsystem->GetSpawnedEntities()[0];
            AActor* SpawnedActor = Runtime.FindActor(RuntimeSpawn.EntityId);
            TestNotNull(TEXT("Spawned Entity maps to a runtime Actor"), SpawnedActor);
            if (SpawnedActor)
            {
                TestEqual(TEXT("Enemy uses Spawn Point transform"), SpawnedActor->GetActorLocation(), SpawnPointTransform.GetLocation());
                const UAkUGCEntityBindingComponent* Binding = SpawnedActor->FindComponentByClass<UAkUGCEntityBindingComponent>();
                TestNotNull(TEXT("Spawned enemy has UGC binding"), Binding);
                if (Binding)
                {
                    TestEqual(TEXT("Spawned binding uses Basic Enemy Prefab"), Binding->PrefabId, FName(TEXT("official.unit.basic_enemy")));
                }
            }
        }
    }

    GEngine->DestroyWorldContext(World);
    World->DestroyWorld(false);
    return true;
}

#endif

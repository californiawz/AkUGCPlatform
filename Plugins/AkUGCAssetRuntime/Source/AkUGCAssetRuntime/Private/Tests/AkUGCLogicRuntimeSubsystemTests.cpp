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
        TestTrue(TEXT("Default Edit session initializes"), Session.Initialize(Document).bSucceeded);
        TestTrue(TEXT("Edit session does not execute Game Start"), Subsystem->GetEmittedMessages().IsEmpty());
    }
    {
        FAkUGCSceneRuntime SceneRuntime(World);
        FAkUGCDocumentRuntimeSession Session(
            SceneRuntime,
            Registry,
            Scene.SceneId,
            EAkUGCRuntimeSessionMode::PlayClient);
        TestTrue(TEXT("Play Client session initializes"), Session.Initialize(Document).bSucceeded);
        TestTrue(TEXT("Play Client session does not execute Game Start"), Subsystem->GetEmittedMessages().IsEmpty());
    }
    {
        FAkUGCSceneRuntime SceneRuntime(World);
        FAkUGCDocumentRuntimeSession Session(
            SceneRuntime,
            Registry,
            Scene.SceneId,
            EAkUGCRuntimeSessionMode::Preview);
        TestTrue(TEXT("Preview session runs Game Start"), Session.Initialize(Document).bSucceeded);
        TestEqual(TEXT("Preview emits the scene message"), Subsystem->GetEmittedMessages().Num(), 1);
    }
    {
        FAkUGCSceneRuntime SceneRuntime(World);
        FAkUGCDocumentRuntimeSession Session(
            SceneRuntime,
            Registry,
            Scene.SceneId,
            EAkUGCRuntimeSessionMode::PlayAuthority);
        TestTrue(TEXT("Play Authority session runs Game Start"), Session.Initialize(Document).bSucceeded);
        TestEqual(TEXT("Play Authority emits the scene message"), Subsystem->GetEmittedMessages().Num(), 1);
    }
    TestTrue(TEXT("Scene unload clears Logic Runtime messages"), Subsystem->GetEmittedMessages().IsEmpty());

    UWorld* EditorWorld = NewObject<UWorld>(GetTransientPackage(), NAME_None, RF_Transient);
    EditorWorld->WorldType = EWorldType::Editor;
    FWorldContext& EditorWorldContext = GEngine->CreateNewWorldContext(EditorWorld->WorldType);
    EditorWorldContext.SetCurrentWorld(EditorWorld);
    EditorWorld->InitializeNewWorld(UWorld::InitializationValues()
        .InitializeScenes(false)
        .AllowAudioPlayback(false)
        .RequiresHitProxies(false)
        .CreatePhysicsScene(false)
        .CreateNavigation(false)
        .CreateAISystem(false)
        .ShouldSimulatePhysics(false)
        .EnableTraceCollision(false)
        .SetTransactional(false));
    {
        FAkUGCSceneRuntime PreviewRuntime(EditorWorld);
        FAkUGCDocumentRuntimeSession PreviewSession(
            PreviewRuntime,
            Registry,
            Scene.SceneId,
            EAkUGCRuntimeSessionMode::Preview);
        TestFalse(TEXT("Preview session explicitly rejects an Editor world"),
            PreviewSession.Initialize(Document).bSucceeded);
    }
    GEngine->DestroyWorldContext(EditorWorld);
    EditorWorld->DestroyWorld(false);

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
    FAkUGCComponentRecord* SpawnConfig = SpawnPoint.Components.FindByPredicate([](const FAkUGCComponentRecord& Component)
    {
        return Component.TypeId == TEXT("tower_defense.spawn");
    });
    TestNotNull(TEXT("Enemy Spawn exposes batch configuration"), SpawnConfig);
    if (!SpawnConfig)
    {
        GEngine->DestroyWorldContext(World);
        World->DestroyWorld(false);
        return false;
    }
    SpawnConfig->Properties.FindChecked(TEXT("enemyCount")).IntegerValue = 3;
    SpawnConfig->Properties.FindChecked(TEXT("spawnInterval")).NumberValue = 0.25;
    Scene.Entities.Add(SpawnPoint);

    FAkUGCEntityRecord FirstPathNode;
    FAkUGCEntityRecord SecondPathNode;
    TestTrue(TEXT("First gameplay path node is created"), Registry.CreateEntityRecord(
        TEXT("official.gameplay.path_node"),
        FGuid::NewGuid(),
        FTransform(FVector(500.0, 50.0, 0.0)),
        FirstPathNode,
        &Error));
    TestTrue(TEXT("Second gameplay path node is created"), Registry.CreateEntityRecord(
        TEXT("official.gameplay.path_node"),
        FGuid::NewGuid(),
        FTransform(FVector(1000.0, 50.0, 0.0)),
        SecondPathNode,
        &Error));
    FirstPathNode.Components[0].Properties.FindChecked(TEXT("order")).IntegerValue = 0;
    SecondPathNode.Components[0].Properties.FindChecked(TEXT("order")).IntegerValue = 1;
    Scene.Entities.Add(FirstPathNode);
    Scene.Entities.Add(SecondPathNode);

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
        FAkUGCProjectDocument MissingPathDocument = Document;
        MissingPathDocument.Scenes[0].Entities.RemoveAll([](const FAkUGCEntityRecord& Entity)
        {
            return Entity.PrefabId == TEXT("official.gameplay.path_node");
        });
        FAkUGCSceneRuntime MissingPathRuntime(World);
        FAkUGCDocumentRuntimeSession MissingPathSession(
            MissingPathRuntime,
            Registry,
            Scene.SceneId,
            EAkUGCRuntimeSessionMode::PlayAuthority);
        TestFalse(TEXT("Play Authority rejects Spawn gameplay without a usable path"),
            MissingPathSession.Initialize(MissingPathDocument).bSucceeded);
    }

    {
        FAkUGCSceneRuntime Runtime(World);
        FAkUGCDocumentRuntimeSession Session(
            Runtime,
            Registry,
            Scene.SceneId,
            EAkUGCRuntimeSessionMode::PlayAuthority);
        TestTrue(TEXT("Timer Spawn scene initializes"), Session.Initialize(Document).bSucceeded);
        TestEqual(TEXT("Three authored gameplay actors exist initially"), Runtime.Num(), 3);
        {
            FAkUGCSceneRuntime EditRuntime(World);
            FAkUGCDocumentRuntimeSession EditSession(EditRuntime, Registry, Scene.SceneId);
            TestTrue(TEXT("Concurrent Edit session initializes"), EditSession.Initialize(Document).bSucceeded);
        }
        {
            FAkUGCSceneRuntime ConflictingRuntime(World);
            FAkUGCDocumentRuntimeSession ConflictingSession(
                ConflictingRuntime,
                Registry,
                Scene.SceneId,
                EAkUGCRuntimeSessionMode::PlayAuthority);
            TestFalse(TEXT("Second authority session cannot steal Logic ownership"),
                ConflictingSession.Initialize(Document).bSucceeded);
        }
        TestTrue(TEXT("Other session teardown does not cancel authority Timer"), Subsystem->AdvanceLogicTime(0.5).bSucceeded);
        TestEqual(TEXT("Enemy is not spawned before Timer expires"), Runtime.Num(), 3);
        TestTrue(TEXT("Remaining delay advances successfully"), Subsystem->AdvanceLogicTime(0.5).bSucceeded);
        TestEqual(TEXT("Timer spawns one Basic Enemy"), Runtime.Num(), 4);
        TestEqual(TEXT("Logic Spawn does not modify authored Document"), Document.Scenes[0].Entities.Num(), 3);
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
        TestTrue(TEXT("First batch interval advances"), Subsystem->AdvanceLogicTime(0.25).bSucceeded);
        TestEqual(TEXT("Second configured enemy spawns"), Runtime.Num(), 5);
        TestTrue(TEXT("Second batch interval advances"), Subsystem->AdvanceLogicTime(0.25).bSucceeded);
        TestEqual(TEXT("Third configured enemy spawns"), Runtime.Num(), 6);
        if (Subsystem->GetSpawnedEntities().Num() == 3)
        {
            AActor* FirstEnemy = Runtime.FindActor(Subsystem->GetSpawnedEntities()[0].EntityId);
            TestNotNull(TEXT("First enemy remains in runtime while moving"), FirstEnemy);
            if (FirstEnemy)
            {
                TestEqual(TEXT("First enemy moves 150 units toward first path node"),
                    FirstEnemy->GetActorLocation(),
                    FVector(400.0, 50.0, 0.0));
            }
        }
        TestEqual(TEXT("All configured spawns are observable"), Subsystem->GetSpawnedEntities().Num(), 3);
        TestTrue(TEXT("Large movement delta reaches path end"), Subsystem->AdvanceLogicTime(5.0).bSucceeded);
        TestEqual(TEXT("All three enemies produce GoalReached"), Subsystem->GetGoalReachedEntities().Num(), 3);
        TestEqual(TEXT("No enemy movement remains after reaching path end"), Runtime.GetActiveEnemyMovementCount(), 0);
        for (const FAkUGCLogicRuntimeSpawn& RuntimeSpawn : Subsystem->GetSpawnedEntities())
        {
            AActor* EnemyActor = Runtime.FindActor(RuntimeSpawn.EntityId);
            TestNotNull(TEXT("GoalReached enemy remains available for next gameplay slice"), EnemyActor);
            if (EnemyActor)
            {
                TestEqual(TEXT("GoalReached enemy stops at final path node"),
                    EnemyActor->GetActorLocation(),
                    SecondPathNode.Transform.GetLocation());
            }
        }
        TestTrue(TEXT("Extra time does not produce duplicate GoalReached"), Subsystem->AdvanceLogicTime(1.0).bSucceeded);
        TestEqual(TEXT("GoalReached is emitted exactly once per enemy"), Subsystem->GetGoalReachedEntities().Num(), 3);
        TestEqual(TEXT("Batch stops at configured enemyCount"), Runtime.Num(), 6);
        TestEqual(TEXT("Batch still leaves authored Document unchanged"), Document.Scenes[0].Entities.Num(), 3);
    }

    {
        FAkUGCSceneRuntime Runtime(World);
        {
            FAkUGCDocumentRuntimeSession Session(
                Runtime,
                Registry,
                Scene.SceneId,
                EAkUGCRuntimeSessionMode::PlayAuthority);
            TestTrue(TEXT("Cancellation fixture initializes"), Session.Initialize(Document).bSucceeded);
            TestEqual(TEXT("Cancellation fixture waits at Timer"), Runtime.Num(), 3);
        }
        TestTrue(TEXT("Advancing after Session destruction is harmless"), Subsystem->AdvanceLogicTime(1.0).bSucceeded);
        TestEqual(TEXT("Session destruction cancels Timer and Spawn Batch"), Runtime.Num(), 3);
    }
    {
        FAkUGCSceneRuntime Runtime(World);
        FAkUGCDocumentRuntimeSession Session(
            Runtime,
            Registry,
            Scene.SceneId,
            EAkUGCRuntimeSessionMode::PlayAuthority);
        TestTrue(TEXT("Large delta fixture initializes"), Session.Initialize(Document).bSucceeded);
        TestTrue(TEXT("Large delta advances Timer and all batch intervals"), Subsystem->AdvanceLogicTime(1.5).bSucceeded);
        TestEqual(TEXT("Large delta deterministically catches up all configured enemies"), Runtime.Num(), 6);
        TestEqual(TEXT("Large delta records all configured enemies"), Subsystem->GetSpawnedEntities().Num(), 3);
    }

    GEngine->DestroyWorldContext(World);
    World->DestroyWorld(false);
    return true;
}

#endif

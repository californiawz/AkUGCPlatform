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
    Document.Manifest.TemplateId = TEXT("official.logic_test");
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
    for (int32 WaveIndex = 0; WaveIndex < AkUGCTowerDefenseRulesetLimits::RequiredWaveCount; ++WaveIndex)
    {
        FAkUGCTowerDefenseWave& Wave = Scene.Ruleset.Waves.AddDefaulted_GetRef();
        Wave.WaveId = FGuid::NewGuid();
        Wave.SpawnPointEntityId = SpawnPointId;
        Wave.StartDelaySeconds = WaveIndex;
    }

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

    const FGuid BaseEntityId = FGuid::NewGuid();
    const FGuid GoalEntityId = FGuid::NewGuid();
    FAkUGCEntityRecord BaseEntity;
    FAkUGCEntityRecord GoalEntity;
    TestTrue(TEXT("Tower defense Base is created"), Registry.CreateEntityRecord(
        TEXT("official.gameplay.base"),
        BaseEntityId,
        FTransform(FVector(1100.0, 50.0, 0.0)),
        BaseEntity,
        &Error));
    TestTrue(TEXT("Tower defense Goal is created"), Registry.CreateEntityRecord(
        TEXT("official.gameplay.goal"),
        GoalEntityId,
        FTransform(FVector(1000.0, 50.0, 0.0)),
        GoalEntity,
        &Error));
    BaseEntity.Components[0].Properties.FindChecked(TEXT("maxHealth")).NumberValue = 25.0;
    GoalEntity.Components[0].Properties.FindChecked(TEXT("baseDamage")).NumberValue = 99.0;
    Scene.Entities.Add(BaseEntity);
    Scene.Entities.Add(GoalEntity);

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
        FAkUGCProjectDocument NoSpawnNodeDocument = Document;
        NoSpawnNodeDocument.Scenes[0].LogicGraph.Nodes.RemoveAll([](const FAkUGCLogicNode& Node)
        {
            return Node.Type == EAkUGCLogicNodeType::Spawn || Node.Type == EAkUGCLogicNodeType::Timer;
        });
        NoSpawnNodeDocument.Scenes[0].LogicGraph.Connections.Reset();
        NoSpawnNodeDocument.Scenes[0].Ruleset.Waves.Reset();
        FAkUGCSceneRuntime NoSpawnNodeRuntime(World);
        FAkUGCDocumentRuntimeSession NoSpawnNodeSession(
            NoSpawnNodeRuntime,
            Registry,
            Scene.SceneId,
            EAkUGCRuntimeSessionMode::PlayAuthority);
        TestFalse(TEXT("Tower defense Play Authority cannot bypass Ruleset validation by removing Spawn nodes"),
            NoSpawnNodeSession.Initialize(NoSpawnNodeDocument).bSucceeded);
    }
    {
        FAkUGCProjectDocument IncompleteRulesetDocument = Document;
        IncompleteRulesetDocument.Scenes[0].Ruleset.Waves.Pop();
        FAkUGCSceneRuntime EditRuntime(World);
        FAkUGCDocumentRuntimeSession EditSession(EditRuntime, Registry, Scene.SceneId);
        TestTrue(TEXT("Edit accepts incomplete Ruleset as an authoring state"),
            EditSession.Initialize(IncompleteRulesetDocument).bSucceeded);
    }
    {
        FAkUGCProjectDocument IncompleteRulesetDocument = Document;
        IncompleteRulesetDocument.Scenes[0].Ruleset.Waves.Pop();
        FAkUGCSceneRuntime PreviewRuntime(World);
        FAkUGCDocumentRuntimeSession PreviewSession(
            PreviewRuntime,
            Registry,
            Scene.SceneId,
            EAkUGCRuntimeSessionMode::Preview);
        TestFalse(TEXT("Preview rejects an incomplete three-wave Ruleset"),
            PreviewSession.Initialize(IncompleteRulesetDocument).bSucceeded);
    }
    {
        FAkUGCProjectDocument IncompleteRulesetDocument = Document;
        IncompleteRulesetDocument.Scenes[0].Ruleset.Waves.Pop();
        FAkUGCSceneRuntime AuthorityRuntime(World);
        FAkUGCDocumentRuntimeSession AuthoritySession(
            AuthorityRuntime,
            Registry,
            Scene.SceneId,
            EAkUGCRuntimeSessionMode::PlayAuthority);
        TestFalse(TEXT("Play Authority rejects an incomplete three-wave Ruleset"),
            AuthoritySession.Initialize(IncompleteRulesetDocument).bSucceeded);
    }
    {
        FAkUGCProjectDocument OverBudgetDocument = Document;
        FAkUGCComponentRecord* OverBudgetSpawn = OverBudgetDocument.Scenes[0].Entities[0].Components.FindByPredicate(
            [](const FAkUGCComponentRecord& Component)
            {
                return Component.TypeId == TEXT("tower_defense.spawn");
            });
        if (OverBudgetSpawn)
        {
            OverBudgetSpawn->Properties.FindChecked(TEXT("enemyCount")).IntegerValue = 200;
        }
        FAkUGCSceneRuntime OverBudgetRuntime(World);
        FAkUGCDocumentRuntimeSession OverBudgetSession(
            OverBudgetRuntime,
            Registry,
            Scene.SceneId,
            EAkUGCRuntimeSessionMode::PlayAuthority);
        TestFalse(TEXT("Play Authority rejects Ruleset total enemy budget overflow"),
            OverBudgetSession.Initialize(OverBudgetDocument).bSucceeded);
    }
    {
        FAkUGCProjectDocument InvalidEnemyDocument = Document;
        FAkUGCComponentRecord* InvalidEnemySpawn = InvalidEnemyDocument.Scenes[0].Entities[0].Components.FindByPredicate(
            [](const FAkUGCComponentRecord& Component)
            {
                return Component.TypeId == TEXT("tower_defense.spawn");
            });
        if (InvalidEnemySpawn)
        {
            InvalidEnemySpawn->Properties.FindChecked(TEXT("enemyPrefab")).NameValue = TEXT("official.gameplay.base");
        }
        FAkUGCSceneRuntime InvalidEnemyRuntime(World);
        FAkUGCDocumentRuntimeSession InvalidEnemySession(
            InvalidEnemyRuntime,
            Registry,
            Scene.SceneId,
            EAkUGCRuntimeSessionMode::PlayAuthority);
        TestFalse(TEXT("Play Authority rejects a Wave Prefab without enemy capabilities"),
            InvalidEnemySession.Initialize(InvalidEnemyDocument).bSucceeded);
    }
    {
        FAkUGCProjectDocument MismatchedSpawnDocument = Document;
        FAkUGCEntityRecord AlternateSpawn = SpawnPoint;
        AlternateSpawn.EntityId = FGuid::NewGuid();
        MismatchedSpawnDocument.Scenes[0].Entities.Add(AlternateSpawn);
        FAkUGCLogicNode* MismatchedSpawnNode = MismatchedSpawnDocument.Scenes[0].LogicGraph.Nodes.FindByPredicate(
            [](const FAkUGCLogicNode& Node)
            {
                return Node.Type == EAkUGCLogicNodeType::Spawn;
            });
        if (MismatchedSpawnNode)
        {
            MismatchedSpawnNode->SpawnAtEntityId = AlternateSpawn.EntityId;
        }
        FAkUGCSceneRuntime MismatchedSpawnRuntime(World);
        FAkUGCDocumentRuntimeSession MismatchedSpawnSession(
            MismatchedSpawnRuntime,
            Registry,
            Scene.SceneId,
            EAkUGCRuntimeSessionMode::PlayAuthority);
        TestFalse(TEXT("Play Authority rejects Logic Spawn outside the Ruleset"),
            MismatchedSpawnSession.Initialize(MismatchedSpawnDocument).bSucceeded);
    }

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
        FAkUGCProjectDocument MissingBaseDocument = Document;
        MissingBaseDocument.Scenes[0].Entities.RemoveAll([](const FAkUGCEntityRecord& Entity)
        {
            return Entity.PrefabId == TEXT("official.gameplay.base");
        });
        FAkUGCSceneRuntime MissingBaseRuntime(World);
        FAkUGCDocumentRuntimeSession MissingBaseSession(
            MissingBaseRuntime,
            Registry,
            Scene.SceneId,
            EAkUGCRuntimeSessionMode::PlayAuthority);
        TestFalse(TEXT("Play Authority rejects Spawn gameplay without a Base"),
            MissingBaseSession.Initialize(MissingBaseDocument).bSucceeded);
    }
    {
        FAkUGCProjectDocument MissingGoalDocument = Document;
        MissingGoalDocument.Scenes[0].Entities.RemoveAll([](const FAkUGCEntityRecord& Entity)
        {
            return Entity.PrefabId == TEXT("official.gameplay.goal");
        });
        FAkUGCSceneRuntime MissingGoalRuntime(World);
        FAkUGCDocumentRuntimeSession MissingGoalSession(
            MissingGoalRuntime,
            Registry,
            Scene.SceneId,
            EAkUGCRuntimeSessionMode::PlayAuthority);
        TestFalse(TEXT("Play Authority rejects Spawn gameplay without a Goal"),
            MissingGoalSession.Initialize(MissingGoalDocument).bSucceeded);
    }
    {
        FAkUGCProjectDocument DuplicateBaseDocument = Document;
        FAkUGCEntityRecord DuplicateBase = BaseEntity;
        DuplicateBase.EntityId = FGuid::NewGuid();
        DuplicateBaseDocument.Scenes[0].Entities.Add(DuplicateBase);
        FAkUGCSceneRuntime DuplicateBaseRuntime(World);
        FAkUGCDocumentRuntimeSession DuplicateBaseSession(
            DuplicateBaseRuntime,
            Registry,
            Scene.SceneId,
            EAkUGCRuntimeSessionMode::PlayAuthority);
        TestFalse(TEXT("Play Authority rejects more than one Base"),
            DuplicateBaseSession.Initialize(DuplicateBaseDocument).bSucceeded);
    }

    {
        FAkUGCSceneRuntime Runtime(World);
        FAkUGCDocumentRuntimeSession Session(
            Runtime,
            Registry,
            Scene.SceneId,
            EAkUGCRuntimeSessionMode::PlayAuthority);
        const FAkUGCCommandExecutionResult InitializeResult = Session.Initialize(Document);
        if (!InitializeResult.bSucceeded)
        {
            AddError(FString::Printf(TEXT("Timer Spawn initialization failed at %s: %s"),
                *InitializeResult.ErrorPath,
                *InitializeResult.ErrorMessage));
            GEngine->DestroyWorldContext(World);
            World->DestroyWorld(false);
            return false;
        }
        TestEqual(TEXT("Five authored gameplay actors exist initially"), Runtime.Num(), 5);
        TestEqual(TEXT("Base runtime health initializes from authored maxHealth"), Runtime.GetBaseCurrentHealth(), 25.0);
        FAkUGCLogicRuntimeHealth BaseHealth;
        TestTrue(TEXT("Blueprint runtime exposes Base health"), Subsystem->GetRuntimeHealth(BaseEntityId, BaseHealth));
        TestEqual(TEXT("Blueprint Base health reports authored maximum"), BaseHealth.Maximum, 25.0);
        TestEqual(TEXT("Blueprint Base health starts full"), BaseHealth.Current, 25.0);
        TestEqual(TEXT("Tower defense Base ID is observable"), Runtime.GetTowerDefenseBaseEntityId(), BaseEntityId);
        TestEqual(TEXT("Tower defense Goal ID is observable"), Runtime.GetTowerDefenseGoalEntityId(), GoalEntityId);
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
        TestEqual(TEXT("Enemy is not spawned before Timer expires"), Runtime.Num(), 5);
        TestTrue(TEXT("Remaining delay advances successfully"), Subsystem->AdvanceLogicTime(0.5).bSucceeded);
        TestEqual(TEXT("Timer spawns one Basic Enemy"), Runtime.Num(), 6);
        TestEqual(TEXT("Logic Spawn does not modify authored Document"), Document.Scenes[0].Entities.Num(), 5);
        TestEqual(TEXT("Spawn result is observable"), Subsystem->GetSpawnedEntities().Num(), 1);
        if (Subsystem->GetSpawnedEntities().Num() == 1)
        {
            const FAkUGCLogicRuntimeSpawn RuntimeSpawn = Subsystem->GetSpawnedEntities()[0];
            FAkUGCLogicRuntimeHealth EnemyHealth;
            TestTrue(TEXT("Spawned enemy exposes Runtime Health"),
                Subsystem->GetRuntimeHealth(RuntimeSpawn.EntityId, EnemyHealth));
            TestEqual(TEXT("Enemy Runtime Health initializes maximum from core.health"), EnemyHealth.Maximum, 100.0);
            TestEqual(TEXT("Enemy Runtime Health starts full"), EnemyHealth.Current, 100.0);
            TestFalse(TEXT("Spawned enemy starts alive"), EnemyHealth.bIsDead);
            double RemainingPathDistance = 0.0;
            TestTrue(TEXT("Spawned enemy exposes remaining path distance"),
                Runtime.GetEnemyRemainingPathDistance(RuntimeSpawn.EntityId, RemainingPathDistance));
            TestEqual(TEXT("Remaining path distance includes all untraversed segments"), RemainingPathDistance, 750.0);
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
        TestEqual(TEXT("Second configured enemy spawns"), Runtime.Num(), 7);
        TestTrue(TEXT("Second batch interval advances"), Subsystem->AdvanceLogicTime(0.25).bSucceeded);
        TestEqual(TEXT("Third configured enemy spawns"), Runtime.Num(), 8);
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
        TestEqual(TEXT("Three enemies clamp Base health to zero"), Runtime.GetBaseCurrentHealth(), 0.0);
        double TotalAppliedDamage = 0.0;
        const TArray<FAkUGCLogicRuntimeGoalReached> GoalReachedEntities = Subsystem->GetGoalReachedEntities();
        for (int32 GoalReachedIndex = 0; GoalReachedIndex < GoalReachedEntities.Num(); ++GoalReachedIndex)
        {
            const FAkUGCLogicRuntimeGoalReached& GoalReached = GoalReachedEntities[GoalReachedIndex];
            TestEqual(TEXT("GoalReached identifies authored Goal"), GoalReached.GoalEntityId, GoalEntityId);
            TestEqual(TEXT("GoalReached identifies authored Base"), GoalReached.BaseEntityId, BaseEntityId);
            TestEqual(TEXT("GoalReached preserves Spawn source node"), GoalReached.SourceNodeId, Spawn.NodeId);
            TestEqual(TEXT("Staggered enemies reach Goal in Spawn order"),
                GoalReached.EntityId,
                Subsystem->GetSpawnedEntities()[GoalReachedIndex].EntityId);
            TotalAppliedDamage += GoalReached.DamageApplied;
        }
        TestEqual(TEXT("Applied damage is clamped to remaining Base health"), TotalAppliedDamage, 25.0);
        TestEqual(TEXT("Each GoalReached emits a standard damage event"), Subsystem->GetDamageEvents().Num(), 3);
        TestEqual(TEXT("Base death is emitted exactly once"), Subsystem->GetDeathEvents().Num(), 1);
        if (Subsystem->GetDeathEvents().Num() == 1)
        {
            TestEqual(TEXT("Death event identifies Base"), Subsystem->GetDeathEvents()[0].EntityId, BaseEntityId);
        }
        TestTrue(TEXT("Zero-health Base remains queryable"), Subsystem->GetRuntimeHealth(BaseEntityId, BaseHealth));
        TestTrue(TEXT("Zero-health Base reports dead"), BaseHealth.bIsDead);
        if (!Subsystem->GetGoalReachedEntities().IsEmpty())
        {
            TestEqual(TEXT("Last GoalReached observes zero Base health"),
                Subsystem->GetGoalReachedEntities().Last().BaseHealthAfterDamage,
                0.0);
        }
        for (const FAkUGCLogicRuntimeSpawn& RuntimeSpawn : Subsystem->GetSpawnedEntities())
        {
            AActor* EnemyActor = Runtime.FindActor(RuntimeSpawn.EntityId);
            TestNull(TEXT("GoalReached enemy is removed from runtime"), EnemyActor);
        }
        TestTrue(TEXT("Extra time does not produce duplicate GoalReached"), Subsystem->AdvanceLogicTime(1.0).bSucceeded);
        TestEqual(TEXT("GoalReached is emitted exactly once per enemy"), Subsystem->GetGoalReachedEntities().Num(), 3);
        TestEqual(TEXT("Extra time does not produce duplicate damage"), Subsystem->GetDamageEvents().Num(), 3);
        TestEqual(TEXT("Extra time does not produce duplicate death"), Subsystem->GetDeathEvents().Num(), 1);
        TestEqual(TEXT("Reached enemies leave only authored runtime actors"), Runtime.Num(), 5);
        TestEqual(TEXT("Batch still leaves authored Document unchanged"), Document.Scenes[0].Entities.Num(), 5);
        const FAkUGCEntityRecord* AuthoredBase = Document.Scenes[0].Entities.FindByPredicate([BaseEntityId](const FAkUGCEntityRecord& Entity)
        {
            return Entity.EntityId == BaseEntityId;
        });
        TestNotNull(TEXT("Authored Base remains in Document"), AuthoredBase);
        if (AuthoredBase)
        {
            const FAkUGCValue* AuthoredMaximumHealth = AuthoredBase->Components[0].Properties.Find(TEXT("maxHealth"));
            TestTrue(TEXT("Runtime damage does not mutate authored Base maxHealth"),
                AuthoredMaximumHealth && AuthoredMaximumHealth->NumberValue == 25.0);
        }
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
            TestTrue(TEXT("Cancellation fixture advances to first Spawn"), Subsystem->AdvanceLogicTime(1.0).bSucceeded);
            TestEqual(TEXT("Cancellation fixture contains one dynamic enemy"), Runtime.Num(), 6);
            TestEqual(TEXT("Cancellation fixture tracks active movement"), Runtime.GetActiveEnemyMovementCount(), 1);
        }
        TestTrue(TEXT("Advancing after Session destruction is harmless"), Subsystem->AdvanceLogicTime(1.0).bSucceeded);
        TestEqual(TEXT("Session destruction removes dynamic enemy and preserves authored actors"), Runtime.Num(), 5);
        TestEqual(TEXT("Session destruction clears active movement"), Runtime.GetActiveEnemyMovementCount(), 0);
        TestEqual(TEXT("Session destruction clears Base runtime health"), Runtime.GetBaseCurrentHealth(), 0.0);
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
        TestEqual(TEXT("Large delta deterministically catches up all configured enemies"), Runtime.Num(), 8);
        TestEqual(TEXT("Large delta records all configured enemies"), Subsystem->GetSpawnedEntities().Num(), 3);
        if (Subsystem->GetSpawnedEntities().Num() == 3)
        {
            const FGuid EnemyEntityId = Subsystem->GetSpawnedEntities()[0].EntityId;
            FAkUGCRuntimeDamage Damage;
            TestTrue(TEXT("Partial Runtime Damage succeeds"),
                Runtime.ApplyRuntimeDamage(BaseEntityId, EnemyEntityId, 40.0, Damage, Error));
            TestEqual(TEXT("Partial Runtime Damage applies requested amount"), Damage.AppliedDamage, 40.0);
            TestEqual(TEXT("Partial Runtime Damage leaves expected health"), Damage.HealthAfterDamage, 60.0);
            TestFalse(TEXT("Partial Runtime Damage does not kill"), Damage.bKilled);
            FAkUGCRuntimeHealth EnemyHealth;
            TestTrue(TEXT("Partially damaged enemy remains queryable"), Runtime.GetRuntimeHealth(EnemyEntityId, EnemyHealth));
            TestEqual(TEXT("Partially damaged enemy retains Runtime Health"), EnemyHealth.Current, 60.0);

            TestTrue(TEXT("Lethal Runtime Damage succeeds"),
                Runtime.ApplyRuntimeDamage(BaseEntityId, EnemyEntityId, 100.0, Damage, Error));
            TestEqual(TEXT("Lethal Runtime Damage clamps applied amount"), Damage.AppliedDamage, 60.0);
            TestEqual(TEXT("Lethal Runtime Damage clamps health to zero"), Damage.HealthAfterDamage, 0.0);
            TestTrue(TEXT("Lethal Runtime Damage reports death"), Damage.bKilled);
            TestNull(TEXT("Dead dynamic enemy Actor is removed"), Runtime.FindActor(EnemyEntityId));
            TestFalse(TEXT("Dead dynamic enemy health is removed"), Runtime.GetRuntimeHealth(EnemyEntityId, EnemyHealth));
            TestEqual(TEXT("Dead dynamic enemy leaves active movement"), Runtime.GetActiveEnemyMovementCount(), 2);
            TestFalse(TEXT("Repeated damage cannot re-kill a removed enemy"),
                Runtime.ApplyRuntimeDamage(BaseEntityId, EnemyEntityId, 1.0, Damage, Error));
        }
    }
    {
        FAkUGCProjectDocument TowerDocument = Document;
        FAkUGCSceneDocument& TowerScene = TowerDocument.Scenes[0];
        FAkUGCComponentRecord* TowerSpawnConfig = TowerScene.Entities[0].Components.FindByPredicate([](const FAkUGCComponentRecord& Component)
        {
            return Component.TypeId == TEXT("tower_defense.spawn");
        });
        TestNotNull(TEXT("Tower fixture finds Spawn configuration"), TowerSpawnConfig);
        if (TowerSpawnConfig)
        {
            TowerSpawnConfig->Properties.FindChecked(TEXT("enemyCount")).IntegerValue = 2;
        }

        const FGuid TowerEntityId = FGuid::NewGuid();
        FAkUGCEntityRecord TowerEntity;
        TestTrue(TEXT("Basic Tower record is created"), Registry.CreateEntityRecord(
            TEXT("official.tower.basic"),
            TowerEntityId,
            FTransform(FVector(600.0, 50.0, 0.0)),
            TowerEntity,
            &Error));
        FAkUGCComponentRecord* TowerConfig = TowerEntity.Components.FindByPredicate([](const FAkUGCComponentRecord& Component)
        {
            return Component.TypeId == TEXT("tower_defense.tower");
        });
        TestNotNull(TEXT("Basic Tower exposes attack configuration"), TowerConfig);
        if (TowerConfig)
        {
            TowerConfig->Properties.FindChecked(TEXT("attackRange")).NumberValue = 1000.0;
            TowerConfig->Properties.FindChecked(TEXT("attackInterval")).NumberValue = 0.5;
            TowerConfig->Properties.FindChecked(TEXT("attackDamage")).NumberValue = 60.0;
        }
        TowerScene.Entities.Add(TowerEntity);

        FAkUGCEntityRecord SupportTowerEntity = TowerEntity;
        SupportTowerEntity.EntityId = FGuid::NewGuid();
        SupportTowerEntity.Transform.SetLocation(FVector(650.0, 50.0, 0.0));
        FAkUGCComponentRecord* SupportTowerConfig = SupportTowerEntity.Components.FindByPredicate([](const FAkUGCComponentRecord& Component)
        {
            return Component.TypeId == TEXT("tower_defense.tower");
        });
        if (SupportTowerConfig)
        {
            SupportTowerConfig->Properties.FindChecked(TEXT("attackDamage")).NumberValue = 0.0;
        }
        TowerScene.Entities.Add(SupportTowerEntity);

        {
            FAkUGCSceneRuntime Runtime(World);
            FAkUGCDocumentRuntimeSession Session(
                Runtime,
                Registry,
                Scene.SceneId,
                EAkUGCRuntimeSessionMode::PlayAuthority);
            TestTrue(TEXT("Basic Tower gameplay initializes"), Session.Initialize(TowerDocument).bSucceeded);
            TestTrue(TEXT("Large delta advances deterministic tower attacks"), Subsystem->AdvanceLogicTime(3.0).bSucceeded);
            TestEqual(TEXT("Tower kills both configured enemies"), Runtime.GetActiveEnemyMovementCount(), 0);
            TestEqual(TEXT("Tower produces four deterministic damage events"), Subsystem->GetDamageEvents().Num(), 4);
            TestEqual(TEXT("Tower produces one death per enemy"), Subsystem->GetDeathEvents().Num(), 2);
            TestEqual(TEXT("Tower kills enemies before GoalReached"), Subsystem->GetGoalReachedEntities().Num(), 0);
            TestEqual(TEXT("Tower defense leaves Base health unchanged"), Runtime.GetBaseCurrentHealth(), 25.0);
            const TArray<FAkUGCLogicRuntimeSpawn> TowerSpawns = Subsystem->GetSpawnedEntities();
            const TArray<FAkUGCLogicRuntimeDamage> TowerDamageEvents = Subsystem->GetDamageEvents();
            for (const FAkUGCLogicRuntimeDamage& Damage : TowerDamageEvents)
            {
                TestEqual(TEXT("Tower damage records source EntityId"), Damage.SourceEntityId, TowerEntityId);
            }
            if (TowerSpawns.Num() == 2 && TowerDamageEvents.Num() == 4)
            {
                TestEqual(TEXT("Tower targets enemy closest to Goal first"), TowerDamageEvents[0].TargetEntityId, TowerSpawns[0].EntityId);
                TestEqual(TEXT("Tower finishes first target before retargeting"), TowerDamageEvents[1].TargetEntityId, TowerSpawns[0].EntityId);
                TestEqual(TEXT("Tower retargets the remaining enemy"), TowerDamageEvents[2].TargetEntityId, TowerSpawns[1].EntityId);
                TestEqual(TEXT("Tower finishes the remaining enemy"), TowerDamageEvents[3].TargetEntityId, TowerSpawns[1].EntityId);
            }
            TestEqual(TEXT("Dead enemies are removed while authored Towers remain"), Runtime.Num(), 7);
        }
        {
            FAkUGCSceneRuntime Runtime(World);
            FAkUGCDocumentRuntimeSession Session(
                Runtime,
                Registry,
                Scene.SceneId,
                EAkUGCRuntimeSessionMode::PlayAuthority);
            TestTrue(TEXT("Small delta tower gameplay initializes"), Session.Initialize(TowerDocument).bSucceeded);
            for (int32 StepIndex = 0; StepIndex < 12; ++StepIndex)
            {
                TestTrue(TEXT("Small delta advances deterministic tower attacks"),
                    Subsystem->AdvanceLogicTime(0.25).bSucceeded);
            }
            TestEqual(TEXT("Small deltas match large delta enemy result"), Runtime.GetActiveEnemyMovementCount(), 0);
            TestEqual(TEXT("Small deltas match large delta damage count"), Subsystem->GetDamageEvents().Num(), 4);
            TestEqual(TEXT("Small deltas match large delta death count"), Subsystem->GetDeathEvents().Num(), 2);
            TestEqual(TEXT("Small deltas match large delta Goal result"), Subsystem->GetGoalReachedEntities().Num(), 0);
            TestEqual(TEXT("Small deltas match large delta Base health"), Runtime.GetBaseCurrentHealth(), 25.0);
        }
    }

    GEngine->DestroyWorldContext(World);
    World->DestroyWorld(false);
    return true;
}

#endif

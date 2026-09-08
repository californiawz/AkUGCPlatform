#include "Misc/AutomationTest.h"

#include "Document/AkUGCDocumentJson.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Subsystem/AkUGCAppEditorSubsystem.h"
#include "Subsystem/AkUGCLogicRuntimeSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCAppEditorSubsystemWorkflowTest,
    "AkUGC.Runtime.AppEditor.BlueprintWorkflow",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCAppEditorSubsystemWorkflowTest::RunTest(const FString& Parameters)
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

    UAkUGCAppEditorSubsystem* Subsystem = World->GetSubsystem<UAkUGCAppEditorSubsystem>();
    TestNotNull(TEXT("App Editor world subsystem exists"), Subsystem);
    if (!Subsystem)
    {
        GEngine->DestroyWorldContext(World);
        World->DestroyWorld(false);
        return false;
    }

    TestTrue(TEXT("App creates tower defense project"), Subsystem->NewTowerDefenseProject().bSucceeded);
    TestTrue(TEXT("App project is open"), Subsystem->HasOpenProject());
    TestEqual(TEXT("App exposes official prefabs"), Subsystem->GetAvailablePrefabIds().Num(), 11);

    FAkUGCProjectDocument LogicDocument = Subsystem->GetDocument();
    FAkUGCLogicNode GameStart;
    GameStart.NodeId = FGuid::NewGuid();
    GameStart.Type = EAkUGCLogicNodeType::GameStart;
    FAkUGCLogicNode Message;
    Message.NodeId = FGuid::NewGuid();
    Message.Type = EAkUGCLogicNodeType::Message;
    Message.Message = TEXT("Must not run while editing");
    FAkUGCLogicConnection LogicConnection;
    LogicConnection.SourceNodeId = GameStart.NodeId;
    LogicConnection.TargetNodeId = Message.NodeId;
    LogicDocument.Scenes[0].LogicGraph.Nodes = {GameStart, Message};
    LogicDocument.Scenes[0].LogicGraph.Connections = {LogicConnection};
    FString EditModeJson;
    FString EditModeError;
    TestTrue(TEXT("App edit-mode logic fixture serializes"),
        FAkUGCDocumentJson::Serialize(LogicDocument, EditModeJson, &EditModeError));
    UAkUGCLogicRuntimeSubsystem* LogicRuntime = World->GetSubsystem<UAkUGCLogicRuntimeSubsystem>();
    TestNotNull(TEXT("Logic Runtime subsystem exists beside App Editor"), LogicRuntime);
    TestTrue(TEXT("App loads project containing Game Start logic"), Subsystem->LoadProjectJson(EditModeJson).bSucceeded);
    if (LogicRuntime)
    {
        TestTrue(TEXT("App Edit session does not execute Game Start"), LogicRuntime->GetEmittedMessages().IsEmpty());
    }

    FGuid EnemyId;
    const FAkUGCAppEditResult PlaceResult = Subsystem->PlacePrefab(
        TEXT("official.unit.basic_enemy"),
        FTransform(FVector(100.0, 0.0, 0.0)),
        EnemyId);
    TestTrue(TEXT("App places an official prefab"), PlaceResult.bSucceeded);
    if (!PlaceResult.bSucceeded)
    {
        Subsystem->CloseProject();
        GEngine->DestroyWorldContext(World);
        World->DestroyWorld(false);
        return false;
    }

    FGuid InvalidScaleEntityId;
    TestFalse(TEXT("App rejects scaling for a fixed-scale prefab"), Subsystem->PlacePrefab(
        TEXT("official.unit.basic_enemy"),
        FTransform(FQuat::Identity, FVector::ZeroVector, FVector(2.0)),
        InvalidScaleEntityId).bSucceeded);
    TestFalse(TEXT("Rejected placement returns invalid EntityId"), InvalidScaleEntityId.IsValid());

    FAkUGCEntityRecord Enemy;
    TestTrue(TEXT("App reads placed entity"), Subsystem->GetEntityRecord(EnemyId, Enemy));
    TArray<FAkUGCPropertyDefinition> MobileProperties;
    TestTrue(TEXT("App reads mobile-editable schema"), Subsystem->GetEditableProperties(EnemyId, MobileProperties));
    TestFalse(TEXT("App schema hides desktop-only team ID"), MobileProperties.ContainsByPredicate([](const FAkUGCPropertyDefinition& Property)
    {
        return Property.PropertyId == TEXT("teamId");
    }));

    FAkUGCValue TeamId;
    TeamId.Type = EAkUGCValueType::Integer;
    TeamId.IntegerValue = 3;
    TestFalse(TEXT("App rejects desktop-only property edit"), Subsystem->SetEntityProperty(
        EnemyId, TEXT("core.team"), TEXT("teamId"), TeamId).bSucceeded);

    FAkUGCValue MaxHealth;
    MaxHealth.Type = EAkUGCValueType::Number;
    MaxHealth.NumberValue = 250.0;
    TestTrue(TEXT("App edits mobile property"), Subsystem->SetEntityProperty(
        EnemyId, TEXT("core.health"), TEXT("maxHealth"), MaxHealth).bSucceeded);
    TestTrue(TEXT("App undo is available"), Subsystem->CanUndo());
    TestTrue(TEXT("App undo succeeds"), Subsystem->Undo().bSucceeded);
    TestTrue(TEXT("App redo succeeds"), Subsystem->Redo().bSucceeded);

    FString Json;
    TestTrue(TEXT("App exports project JSON"), Subsystem->ExportProjectJson(Json).bSucceeded);
    TestFalse(TEXT("Exported project JSON is not empty"), Json.IsEmpty());

    const FString MaliciousJson = Json.Replace(
        TEXT("\"integerValue\": \"2\""),
        TEXT("\"integerValue\": \"3\""),
        ESearchCase::CaseSensitive);
    TestNotEqual(TEXT("Desktop-only property was modified in JSON fixture"), MaliciousJson, Json);
    TestFalse(TEXT("App rejects JSON that modifies a desktop-only property"), Subsystem->LoadProjectJson(MaliciousJson).bSucceeded);
    TestTrue(TEXT("Rejected JSON load preserves current project"), Subsystem->GetEntityRecord(EnemyId, Enemy));

    FAkUGCProjectDocument MissingReadonlyDocument;
    FString FixtureError;
    TestTrue(TEXT("Exported JSON parses for readonly deletion fixture"), FAkUGCDocumentJson::Deserialize(
        Json,
        MissingReadonlyDocument,
        &FixtureError));
    if (MissingReadonlyDocument.Scenes.IsEmpty()
        || MissingReadonlyDocument.Scenes[0].Entities.IsEmpty())
    {
        AddError(TEXT("Readonly deletion fixture does not contain the expected entity."));
        Subsystem->CloseProject();
        GEngine->DestroyWorldContext(World);
        World->DestroyWorld(false);
        return false;
    }
    FAkUGCEntityRecord& FixtureEntity = MissingReadonlyDocument.Scenes[0].Entities[0];
    FAkUGCComponentRecord* TeamComponent = FixtureEntity.Components.FindByPredicate([](const FAkUGCComponentRecord& Component)
    {
        return Component.TypeId == TEXT("core.team");
    });
    if (!TeamComponent)
    {
        AddError(TEXT("Readonly deletion fixture does not contain core.team."));
        Subsystem->CloseProject();
        GEngine->DestroyWorldContext(World);
        World->DestroyWorld(false);
        return false;
    }
    TeamComponent->Properties.Remove(TEXT("teamId"));
    FString MissingReadonlyJson;
    TestTrue(TEXT("Readonly deletion fixture serializes"), FAkUGCDocumentJson::Serialize(
        MissingReadonlyDocument,
        MissingReadonlyJson,
        &FixtureError));
    TestFalse(TEXT("App rejects JSON that removes a desktop-only property"), Subsystem->LoadProjectJson(MissingReadonlyJson).bSucceeded);

    FAkUGCProjectDocument OverBudgetDocument = Subsystem->GetDocument();
    const FAkUGCEntityRecord TemplateEntity = OverBudgetDocument.Scenes[0].Entities[0];
    while (OverBudgetDocument.Scenes[0].Entities.Num() <= 500)
    {
        FAkUGCEntityRecord& Extra = OverBudgetDocument.Scenes[0].Entities.Add_GetRef(TemplateEntity);
        Extra.EntityId = FGuid::NewGuid();
    }
    FString OverBudgetJson;
    TestTrue(TEXT("Over-budget fixture serializes"), FAkUGCDocumentJson::Serialize(
        OverBudgetDocument,
        OverBudgetJson,
        &FixtureError));
    TestFalse(TEXT("App rejects JSON beyond mobile content budget"), Subsystem->LoadProjectJson(OverBudgetJson).bSucceeded);

    Subsystem->CloseProject();
    TestFalse(TEXT("Close clears App project"), Subsystem->HasOpenProject());
    TestTrue(TEXT("App reloads exported JSON"), Subsystem->LoadProjectJson(Json).bSucceeded);
    TestTrue(TEXT("Reloaded App project restores entity"), Subsystem->GetEntityRecord(EnemyId, Enemy));

    Subsystem->CloseProject();
    GEngine->DestroyWorldContext(World);
    World->DestroyWorld(false);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCAppEditorLogicEditingTest,
    "AkUGC.Runtime.AppEditor.LogicEditing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCAppEditorLogicEditingTest::RunTest(const FString& Parameters)
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

    UAkUGCAppEditorSubsystem* Subsystem = World->GetSubsystem<UAkUGCAppEditorSubsystem>();
    TestNotNull(TEXT("App Editor world subsystem exists"), Subsystem);
    if (!Subsystem)
    {
        GEngine->DestroyWorldContext(World);
        World->DestroyWorld(false);
        return false;
    }

    TestTrue(TEXT("App creates tower defense project"), Subsystem->NewTowerDefenseProject().bSucceeded);

    FGuid SpawnPointId;
    const FAkUGCAppEditResult SpawnPointPlacement = Subsystem->PlacePrefab(
        TEXT("official.gameplay.enemy_spawn"),
        FTransform(FVector(0.0, 0.0, 0.0)),
        SpawnPointId);
    TestTrue(TEXT("App places an enemy spawn point"), SpawnPointPlacement.bSucceeded);
    if (!SpawnPointPlacement.bSucceeded)
    {
        Subsystem->CloseProject();
        GEngine->DestroyWorldContext(World);
        World->DestroyWorld(false);
        return false;
    }

    // 延迟刷怪模板：GameStart -> Timer -> Spawn
    FAkUGCLogicNode GameStart;
    GameStart.NodeId = FGuid::NewGuid();
    GameStart.Type = EAkUGCLogicNodeType::GameStart;
    TestTrue(TEXT("App adds Game Start node"), Subsystem->AddLogicNode(GameStart).bSucceeded);

    FAkUGCLogicNode Timer;
    Timer.NodeId = FGuid::NewGuid();
    Timer.Type = EAkUGCLogicNodeType::Timer;
    Timer.DelaySeconds = 3.0;
    TestTrue(TEXT("App adds Timer node"), Subsystem->AddLogicNode(Timer).bSucceeded);

    FAkUGCLogicNode Spawn;
    Spawn.NodeId = FGuid::NewGuid();
    Spawn.Type = EAkUGCLogicNodeType::Spawn;
    Spawn.SpawnPrefabId = TEXT("official.unit.basic_enemy");
    Spawn.SpawnAtEntityId = SpawnPointId;
    TestTrue(TEXT("App adds Spawn node anchored to a spawn point"), Subsystem->AddLogicNode(Spawn).bSucceeded);

    TestTrue(TEXT("App connects Game Start to Timer"), Subsystem->ConnectLogicNode(
        GameStart.NodeId, Timer.NodeId).bSucceeded);
    TestTrue(TEXT("App connects Timer to Spawn"), Subsystem->ConnectLogicNode(
        Timer.NodeId, Spawn.NodeId).bSucceeded);

    FAkUGCLogicGraph Graph;
    TestTrue(TEXT("App reads logic graph"), Subsystem->GetLogicGraph(Graph));
    TestEqual(TEXT("App logic graph has three nodes"), Graph.Nodes.Num(), 3);
    TestEqual(TEXT("App logic graph has two connections"), Graph.Connections.Num(), 2);

    TestTrue(TEXT("App undo is available after logic editing"), Subsystem->CanUndo());
    TestTrue(TEXT("App undo succeeds for logic editing"), Subsystem->Undo().bSucceeded);
    TestTrue(TEXT("App redo succeeds for logic editing"), Subsystem->Redo().bSucceeded);

    // 移动端权限：禁止 WaveStart 节点（分波刷怪由 Ruleset 拥有）
    FAkUGCLogicNode WaveStart;
    WaveStart.NodeId = FGuid::NewGuid();
    WaveStart.Type = EAkUGCLogicNodeType::WaveStart;
    TestFalse(TEXT("App rejects Wave Start node"), Subsystem->AddLogicNode(WaveStart).bSucceeded);

    // 移动端权限：Spawn 只能刷官方 enemy
    FAkUGCLogicNode BadSpawn;
    BadSpawn.NodeId = FGuid::NewGuid();
    BadSpawn.Type = EAkUGCLogicNodeType::Spawn;
    BadSpawn.SpawnPrefabId = TEXT("official.gameplay.tower_arrow");
    BadSpawn.SpawnAtEntityId = SpawnPointId;
    TestFalse(TEXT("App rejects non-enemy spawn prefab"), Subsystem->AddLogicNode(BadSpawn).bSucceeded);

    // 移动端权限：Spawn 必须锚定到 enemy_spawn 实体
    FAkUGCLogicNode OrphanSpawn;
    OrphanSpawn.NodeId = FGuid::NewGuid();
    OrphanSpawn.Type = EAkUGCLogicNodeType::Spawn;
    OrphanSpawn.SpawnPrefabId = TEXT("official.unit.basic_enemy");
    OrphanSpawn.SpawnAtEntityId = FGuid::NewGuid();
    TestFalse(TEXT("App rejects spawn without a valid anchor"), Subsystem->AddLogicNode(OrphanSpawn).bSucceeded);

    // 恶意 JSON 拒绝：包含 WaveStart 节点的文档
    FAkUGCProjectDocument MaliciousLogicDocument = Subsystem->GetDocument();
    FAkUGCLogicNode& Injected = MaliciousLogicDocument.Scenes[0].LogicGraph.Nodes.AddDefaulted_GetRef();
    Injected.NodeId = FGuid::NewGuid();
    Injected.Type = EAkUGCLogicNodeType::WaveStart;
    FString MaliciousLogicJson;
    FString FixtureError;
    TestTrue(TEXT("Malicious logic fixture serializes"), FAkUGCDocumentJson::Serialize(
        MaliciousLogicDocument, MaliciousLogicJson, &FixtureError));
    TestFalse(TEXT("App rejects JSON containing a forbidden logic node"), Subsystem->LoadProjectJson(MaliciousLogicJson).bSucceeded);

    // 移动端 Logic 预算：超过 64 个节点
    FAkUGCProjectDocument OverBudgetLogicDocument = Subsystem->GetDocument();
    while (OverBudgetLogicDocument.Scenes[0].LogicGraph.Nodes.Num() <= 64)
    {
        FAkUGCLogicNode& Extra = OverBudgetLogicDocument.Scenes[0].LogicGraph.Nodes.AddDefaulted_GetRef();
        Extra.NodeId = FGuid::NewGuid();
        Extra.Type = EAkUGCLogicNodeType::Message;
        Extra.Message = TEXT("budget");
    }
    FString OverBudgetLogicJson;
    TestTrue(TEXT("Over-budget logic fixture serializes"), FAkUGCDocumentJson::Serialize(
        OverBudgetLogicDocument, OverBudgetLogicJson, &FixtureError));
    TestFalse(TEXT("App rejects JSON beyond mobile logic budget"), Subsystem->LoadProjectJson(OverBudgetLogicJson).bSucceeded);

    // JSON 往返一致性：导出后重载恢复 Logic 图
    FString Json;
    TestTrue(TEXT("App exports project JSON after logic editing"), Subsystem->ExportProjectJson(Json).bSucceeded);
    Subsystem->CloseProject();
    TestTrue(TEXT("App reloads exported JSON"), Subsystem->LoadProjectJson(Json).bSucceeded);
    FAkUGCLogicGraph ReloadedGraph;
    TestTrue(TEXT("Reloaded App reads logic graph"), Subsystem->GetLogicGraph(ReloadedGraph));
    TestEqual(TEXT("Reloaded logic graph restores nodes"), ReloadedGraph.Nodes.Num(), 3);
    TestEqual(TEXT("Reloaded logic graph restores connections"), ReloadedGraph.Connections.Num(), 2);

    Subsystem->CloseProject();
    GEngine->DestroyWorldContext(World);
    World->DestroyWorld(false);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCAppEditorRulesetEditingTest,
    "AkUGC.Runtime.AppEditor.RulesetEditing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCAppEditorRulesetEditingTest::RunTest(const FString& Parameters)
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

    UAkUGCAppEditorSubsystem* Subsystem = World->GetSubsystem<UAkUGCAppEditorSubsystem>();
    TestNotNull(TEXT("App Editor world subsystem exists"), Subsystem);
    if (!Subsystem)
    {
        GEngine->DestroyWorldContext(World);
        World->DestroyWorld(false);
        return false;
    }

    TestTrue(TEXT("App creates tower defense project"), Subsystem->NewTowerDefenseProject().bSucceeded);

    FGuid SpawnPointA;
    FGuid SpawnPointB;
    TestTrue(TEXT("App places first spawn point"), Subsystem->PlacePrefab(
        TEXT("official.gameplay.enemy_spawn"),
        FTransform(FVector(0.0, 0.0, 0.0)),
        SpawnPointA).bSucceeded);
    TestTrue(TEXT("App places second spawn point"), Subsystem->PlacePrefab(
        TEXT("official.gameplay.enemy_spawn"),
        FTransform(FVector(100.0, 0.0, 0.0)),
        SpawnPointB).bSucceeded);

    FGuid EnemyEntityId;
    TestTrue(TEXT("App places an enemy prefab"), Subsystem->PlacePrefab(
        TEXT("official.unit.basic_enemy"),
        FTransform(FVector(200.0, 0.0, 0.0)),
        EnemyEntityId).bSucceeded);

    FAkUGCTowerDefenseWave Wave1;
    Wave1.WaveId = FGuid::NewGuid();
    Wave1.SpawnPointEntityId = SpawnPointA;
    Wave1.StartDelaySeconds = 0.0;
    TestTrue(TEXT("App adds first wave"), Subsystem->AddWave(Wave1).bSucceeded);

    FAkUGCTowerDefenseWave Wave2;
    Wave2.WaveId = FGuid::NewGuid();
    Wave2.SpawnPointEntityId = SpawnPointB;
    Wave2.StartDelaySeconds = 10.0;
    TestTrue(TEXT("App adds second wave"), Subsystem->AddWave(Wave2).bSucceeded);

    FAkUGCTowerDefenseWave Wave3;
    Wave3.WaveId = FGuid::NewGuid();
    Wave3.SpawnPointEntityId = SpawnPointA;
    Wave3.StartDelaySeconds = 20.0;
    TestTrue(TEXT("App adds third wave"), Subsystem->AddWave(Wave3).bSucceeded);

    FAkUGCTowerDefenseWave Wave4;
    Wave4.WaveId = FGuid::NewGuid();
    Wave4.SpawnPointEntityId = SpawnPointA;
    Wave4.StartDelaySeconds = 30.0;
    TestFalse(TEXT("App rejects a fourth wave"), Subsystem->AddWave(Wave4).bSucceeded);

    FAkUGCTowerDefenseWave OrphanWave;
    OrphanWave.WaveId = FGuid::NewGuid();
    OrphanWave.SpawnPointEntityId = FGuid::NewGuid();
    TestFalse(TEXT("App rejects wave with an orphan spawn point"), Subsystem->AddWave(OrphanWave).bSucceeded);

    FAkUGCTowerDefenseWave NonSpawnWave;
    NonSpawnWave.WaveId = FGuid::NewGuid();
    NonSpawnWave.SpawnPointEntityId = EnemyEntityId;
    TestFalse(TEXT("App rejects wave anchored to a non-spawn entity"), Subsystem->AddWave(NonSpawnWave).bSucceeded);

    Wave2.StartDelaySeconds = 15.0;
    TestTrue(TEXT("App updates a wave"), Subsystem->UpdateWave(Wave2).bSucceeded);

    TestTrue(TEXT("App moves a wave"), Subsystem->MoveWave(Wave1.WaveId, 2).bSucceeded);
    TestTrue(TEXT("App deletes a wave"), Subsystem->DeleteWave(Wave3.WaveId).bSucceeded);

    TestTrue(TEXT("App updates ruleset settings"), Subsystem->SetRulesetSettings(
        8.0,
        EAkUGCTowerDefenseDefeatCondition::BaseHealthDepleted,
        EAkUGCTowerDefenseVictoryCondition::AllWavesCleared).bSucceeded);
    TestFalse(TEXT("App rejects negative wave interval"), Subsystem->SetRulesetSettings(
        -1.0,
        EAkUGCTowerDefenseDefeatCondition::BaseHealthDepleted,
        EAkUGCTowerDefenseVictoryCondition::AllWavesCleared).bSucceeded);

    FAkUGCTowerDefenseRuleset Ruleset;
    TestTrue(TEXT("App reads ruleset"), Subsystem->GetRuleset(Ruleset));
    TestEqual(TEXT("App ruleset keeps two waves after edit"), Ruleset.Waves.Num(), 2);
    TestEqual(TEXT("App ruleset stores wave interval"), Ruleset.WaveIntervalSeconds, 8.0);

    TestTrue(TEXT("App undo is available after ruleset editing"), Subsystem->CanUndo());
    TestTrue(TEXT("App undo succeeds for ruleset editing"), Subsystem->Undo().bSucceeded);
    TestTrue(TEXT("App redo succeeds for ruleset editing"), Subsystem->Redo().bSucceeded);

    // 恶意 JSON 拒绝：超过 3 波上限的文档
    FAkUGCProjectDocument OverWaveDocument = Subsystem->GetDocument();
    while (OverWaveDocument.Scenes[0].Ruleset.Waves.Num() < 4)
    {
        FAkUGCTowerDefenseWave& Extra = OverWaveDocument.Scenes[0].Ruleset.Waves.AddDefaulted_GetRef();
        Extra.WaveId = FGuid::NewGuid();
        Extra.SpawnPointEntityId = SpawnPointA;
    }
    FString OverWaveJson;
    FString FixtureError;
    TestTrue(TEXT("Over-wave fixture serializes"), FAkUGCDocumentJson::Serialize(
        OverWaveDocument, OverWaveJson, &FixtureError));
    TestFalse(TEXT("App rejects JSON with more than three waves"), Subsystem->LoadProjectJson(OverWaveJson).bSucceeded);

    // JSON 往返一致：导出后重载恢复 Ruleset
    FString Json;
    TestTrue(TEXT("App exports project JSON after ruleset editing"), Subsystem->ExportProjectJson(Json).bSucceeded);
    Subsystem->CloseProject();
    TestTrue(TEXT("App reloads exported JSON"), Subsystem->LoadProjectJson(Json).bSucceeded);
    FAkUGCTowerDefenseRuleset ReloadedRuleset;
    TestTrue(TEXT("Reloaded App reads ruleset"), Subsystem->GetRuleset(ReloadedRuleset));
    TestEqual(TEXT("Reloaded ruleset restores waves"), ReloadedRuleset.Waves.Num(), 2);

    Subsystem->CloseProject();
    GEngine->DestroyWorldContext(World);
    World->DestroyWorld(false);
    return true;
}

#endif

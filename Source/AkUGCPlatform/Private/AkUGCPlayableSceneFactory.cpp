#include "AkUGCPlayableSceneFactory.h"

#include "Prefab/AkUGCOfficialPrefabCatalog.h"
#include "Prefab/AkUGCPrefabRegistry.h"

bool AkUGCPlayableSceneFactory::MakePlayableTowerDefenseDocument(
    FAkUGCProjectDocument& OutDocument,
    FString* OutError)
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

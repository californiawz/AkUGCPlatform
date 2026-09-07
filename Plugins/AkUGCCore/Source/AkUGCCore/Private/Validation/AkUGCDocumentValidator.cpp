#include "Validation/AkUGCDocumentValidator.h"

#include "Document/AkUGCDocument.h"

namespace
{
    bool HasParentCycle(
        const FGuid& EntityId,
        const TMap<FGuid, FGuid>& ParentByEntity,
        TMap<FGuid, uint8>& VisitStates)
    {
        const uint8 State = VisitStates.FindRef(EntityId);
        if (State == 1)
        {
            return true;
        }
        if (State == 2)
        {
            return false;
        }

        VisitStates.Add(EntityId, 1);
        if (const FGuid* ParentId = ParentByEntity.Find(EntityId))
        {
            if (ParentId->IsValid() && ParentByEntity.Contains(*ParentId)
                && HasParentCycle(*ParentId, ParentByEntity, VisitStates))
            {
                return true;
            }
        }
        VisitStates.Add(EntityId, 2);
        return false;
    }

    bool HasLogicCycle(
        const FGuid& NodeId,
        const TMultiMap<FGuid, FGuid>& TargetsBySource,
        TMap<FGuid, uint8>& VisitStates)
    {
        const uint8 State = VisitStates.FindRef(NodeId);
        if (State == 1)
        {
            return true;
        }
        if (State == 2)
        {
            return false;
        }

        VisitStates.Add(NodeId, 1);
        TArray<FGuid> Targets;
        TargetsBySource.MultiFind(NodeId, Targets);
        for (const FGuid& TargetId : Targets)
        {
            if (HasLogicCycle(TargetId, TargetsBySource, VisitStates))
            {
                return true;
            }
        }
        VisitStates.Add(NodeId, 2);
        return false;
    }
}

FAkUGCValidationResult FAkUGCDocumentValidator::ValidateLogicGraph(
    const FAkUGCLogicGraph& LogicGraph,
    const FString& Path)
{
    FAkUGCValidationResult Result;
    if (LogicGraph.Nodes.Num() > AkUGCLogicLimits::MaxNodes)
    {
        Result.AddError(
            Path + TEXT(".nodes"),
            FString::Printf(TEXT("Logic graph exceeds the node budget of %d."), AkUGCLogicLimits::MaxNodes));
    }
    if (LogicGraph.Connections.Num() > AkUGCLogicLimits::MaxConnections)
    {
        Result.AddError(
            Path + TEXT(".connections"),
            FString::Printf(TEXT("Logic graph exceeds the connection budget of %d."), AkUGCLogicLimits::MaxConnections));
    }
    if (!Result.IsValid())
    {
        return Result;
    }

    TSet<FGuid> NodeIds;
    TMap<FGuid, EAkUGCLogicNodeType> NodeTypes;
    FGuid GameStartId;
    FGuid WaveStartId;
    int32 GameStartCount = 0;
    int32 WaveStartCount = 0;

    for (int32 NodeIndex = 0; NodeIndex < LogicGraph.Nodes.Num(); ++NodeIndex)
    {
        const FAkUGCLogicNode& Node = LogicGraph.Nodes[NodeIndex];
        const FString NodePath = FString::Printf(TEXT("%s.nodes[%d]"), *Path, NodeIndex);
        if (!Node.NodeId.IsValid())
        {
            Result.AddError(NodePath + TEXT(".nodeId"), TEXT("Logic node ID must be a valid GUID."));
        }
        else if (NodeIds.Contains(Node.NodeId))
        {
            Result.AddError(NodePath + TEXT(".nodeId"), TEXT("Logic node ID must be unique in the graph."));
        }
        else
        {
            NodeIds.Add(Node.NodeId);
            NodeTypes.Add(Node.NodeId, Node.Type);
        }

        switch (Node.Type)
        {
        case EAkUGCLogicNodeType::GameStart:
            ++GameStartCount;
            GameStartId = Node.NodeId;
            if (!Node.Message.IsEmpty())
            {
                Result.AddError(NodePath + TEXT(".message"), TEXT("Game Start node cannot contain a message."));
            }
            if (Node.DelaySeconds != 0.0 || !Node.SpawnPrefabId.IsNone() || Node.SpawnAtEntityId.IsValid())
            {
                Result.AddError(NodePath, TEXT("Game Start node contains parameters for another node type."));
            }
            break;

        case EAkUGCLogicNodeType::WaveStart:
            ++WaveStartCount;
            WaveStartId = Node.NodeId;
            if (!Node.Message.IsEmpty()
                || Node.DelaySeconds != 0.0
                || !Node.SpawnPrefabId.IsNone()
                || Node.SpawnAtEntityId.IsValid())
            {
                Result.AddError(NodePath, TEXT("Wave Start node contains parameters for another node type."));
            }
            break;

        case EAkUGCLogicNodeType::Message:
            if (Node.Message.TrimStartAndEnd().IsEmpty())
            {
                Result.AddError(NodePath + TEXT(".message"), TEXT("Message node text is required."));
            }
            if (Node.Message.Len() > AkUGCLogicLimits::MaxMessageLength)
            {
                Result.AddError(
                    NodePath + TEXT(".message"),
                    FString::Printf(TEXT("Message exceeds the length budget of %d characters."), AkUGCLogicLimits::MaxMessageLength));
            }
            if (Node.DelaySeconds != 0.0 || !Node.SpawnPrefabId.IsNone() || Node.SpawnAtEntityId.IsValid())
            {
                Result.AddError(NodePath, TEXT("Message node contains parameters for another node type."));
            }
            break;

        case EAkUGCLogicNodeType::Timer:
            if (!Node.Message.IsEmpty() || !Node.SpawnPrefabId.IsNone() || Node.SpawnAtEntityId.IsValid())
            {
                Result.AddError(NodePath, TEXT("Timer node contains parameters for another node type."));
            }
            if (!FMath::IsFinite(Node.DelaySeconds)
                || Node.DelaySeconds <= 0.0
                || Node.DelaySeconds > AkUGCLogicLimits::MaxTimerDelaySeconds)
            {
                Result.AddError(
                    NodePath + TEXT(".delaySeconds"),
                    FString::Printf(
                        TEXT("Timer delay must be finite, positive, and at most %.0f seconds."),
                        AkUGCLogicLimits::MaxTimerDelaySeconds));
            }
            break;

        case EAkUGCLogicNodeType::Spawn:
            if (!Node.Message.IsEmpty() || Node.DelaySeconds != 0.0)
            {
                Result.AddError(NodePath, TEXT("Spawn node contains parameters for another node type."));
            }
            if (Node.SpawnPrefabId.IsNone())
            {
                Result.AddError(NodePath + TEXT(".spawnPrefabId"), TEXT("Spawn Prefab ID is required."));
            }
            break;

        default:
            Result.AddError(NodePath + TEXT(".type"), TEXT("Logic node type is not supported."));
            break;
        }
    }

    if (GameStartCount > 1)
    {
        Result.AddError(Path + TEXT(".nodes"), TEXT("Logic graph can contain at most one Game Start node."));
    }
    if (WaveStartCount > 1)
    {
        Result.AddError(Path + TEXT(".nodes"), TEXT("Logic graph can contain at most one Wave Start node."));
    }

    TSet<FString> ConnectionKeys;
    TMultiMap<FGuid, FGuid> TargetsBySource;
    TMap<FGuid, int32> IncomingCounts;
    TMap<FGuid, int32> OutgoingCounts;
    for (int32 ConnectionIndex = 0; ConnectionIndex < LogicGraph.Connections.Num(); ++ConnectionIndex)
    {
        const FAkUGCLogicConnection& Connection = LogicGraph.Connections[ConnectionIndex];
        const FString ConnectionPath = FString::Printf(TEXT("%s.connections[%d]"), *Path, ConnectionIndex);
        if (!Connection.SourceNodeId.IsValid() || !NodeIds.Contains(Connection.SourceNodeId))
        {
            Result.AddError(ConnectionPath + TEXT(".sourceNodeId"), TEXT("Connection source node must exist in the graph."));
        }
        if (!Connection.TargetNodeId.IsValid() || !NodeIds.Contains(Connection.TargetNodeId))
        {
            Result.AddError(ConnectionPath + TEXT(".targetNodeId"), TEXT("Connection target node must exist in the graph."));
        }
        if (Connection.SourceNodeId == Connection.TargetNodeId)
        {
            Result.AddError(ConnectionPath, TEXT("Logic node cannot connect to itself."));
        }

        const FString ConnectionKey = Connection.SourceNodeId.ToString(EGuidFormats::Digits)
            + TEXT("->") + Connection.TargetNodeId.ToString(EGuidFormats::Digits);
        if (ConnectionKeys.Contains(ConnectionKey))
        {
            Result.AddError(ConnectionPath, TEXT("Logic connection must be unique."));
        }
        else
        {
            ConnectionKeys.Add(ConnectionKey);
        }

        if (NodeIds.Contains(Connection.SourceNodeId) && NodeIds.Contains(Connection.TargetNodeId))
        {
            TargetsBySource.Add(Connection.SourceNodeId, Connection.TargetNodeId);
            ++OutgoingCounts.FindOrAdd(Connection.SourceNodeId);
            ++IncomingCounts.FindOrAdd(Connection.TargetNodeId);
        }
    }

    for (const TPair<FGuid, EAkUGCLogicNodeType>& Pair : NodeTypes)
    {
        if ((Pair.Value == EAkUGCLogicNodeType::GameStart || Pair.Value == EAkUGCLogicNodeType::WaveStart)
            && IncomingCounts.FindRef(Pair.Key) > 0)
        {
            Result.AddError(Path + TEXT(".connections"), TEXT("Logic event node cannot have incoming connections."));
        }
        if ((Pair.Value == EAkUGCLogicNodeType::Message || Pair.Value == EAkUGCLogicNodeType::Spawn)
            && OutgoingCounts.FindRef(Pair.Key) > 0)
        {
            Result.AddError(Path + TEXT(".connections"), TEXT("Terminal logic node cannot have outgoing connections."));
        }
        if (Pair.Value == EAkUGCLogicNodeType::Timer && OutgoingCounts.FindRef(Pair.Key) != 1)
        {
            Result.AddError(Path + TEXT(".connections"), TEXT("Timer node must have exactly one outgoing connection."));
        }
    }

    TMap<FGuid, uint8> VisitStates;
    for (const FGuid& NodeId : NodeIds)
    {
        if (HasLogicCycle(NodeId, TargetsBySource, VisitStates))
        {
            Result.AddError(Path + TEXT(".connections"), TEXT("Logic graph contains a cycle."));
            break;
        }
    }

    const auto ValidateEntryBudget = [&Result, &TargetsBySource, &Path](
        const FGuid& EntryNodeId,
        const TCHAR* EventName)
    {
        TArray<FGuid> PendingNodes = {EntryNodeId};
        int32 ReadIndex = 0;
        while (ReadIndex < PendingNodes.Num()
            && ReadIndex <= AkUGCLogicLimits::MaxExecutedInstructions)
        {
            TArray<FGuid> Targets;
            TargetsBySource.MultiFind(PendingNodes[ReadIndex++], Targets);
            PendingNodes.Append(Targets);
        }
        if (ReadIndex > AkUGCLogicLimits::MaxExecutedInstructions
            || PendingNodes.Num() > AkUGCLogicLimits::MaxExecutedInstructions)
        {
            Result.AddError(
                Path + TEXT(".connections"),
                FString::Printf(
                    TEXT("%s execution exceeds the instruction budget of %d."),
                    EventName,
                    AkUGCLogicLimits::MaxExecutedInstructions));
        }
    };
    if (Result.IsValid() && GameStartCount == 1)
    {
        ValidateEntryBudget(GameStartId, TEXT("Game Start"));
    }
    if (Result.IsValid() && WaveStartCount == 1)
    {
        ValidateEntryBudget(WaveStartId, TEXT("Wave Start"));
    }
    return Result;
}

bool FAkUGCValidationResult::IsValid() const
{
    return !Issues.ContainsByPredicate([](const FAkUGCValidationIssue& Issue)
    {
        return Issue.Severity == EAkUGCValidationSeverity::Error;
    });
}

void FAkUGCValidationResult::AddError(FString Path, FString Message)
{
    Issues.Add({EAkUGCValidationSeverity::Error, MoveTemp(Path), MoveTemp(Message)});
}

void FAkUGCValidationResult::AddWarning(FString Path, FString Message)
{
    Issues.Add({EAkUGCValidationSeverity::Warning, MoveTemp(Path), MoveTemp(Message)});
}

FAkUGCValidationResult FAkUGCDocumentValidator::Validate(const FAkUGCProjectDocument& Document)
{
    FAkUGCValidationResult Result;

    if (Document.Manifest.SchemaVersion != AkUGCSchema::CurrentProjectDocumentVersion)
    {
        Result.AddError(
            TEXT("manifest.schemaVersion"),
            FString::Printf(
                TEXT("Unsupported schema version %d; expected %d."),
                Document.Manifest.SchemaVersion,
                AkUGCSchema::CurrentProjectDocumentVersion));
    }

    if (!Document.Manifest.ProjectId.IsValid())
    {
        Result.AddError(TEXT("manifest.projectId"), TEXT("Project ID must be a valid GUID."));
    }

    if (Document.Manifest.DisplayName.TrimStartAndEnd().IsEmpty())
    {
        Result.AddError(TEXT("manifest.displayName"), TEXT("Project display name is required."));
    }

    if (Document.Manifest.TemplateId.IsNone())
    {
        Result.AddError(TEXT("manifest.templateId"), TEXT("Template ID is required."));
    }

    TSet<FGuid> SceneIds;
    TSet<FGuid> EntityIds;

    for (int32 SceneIndex = 0; SceneIndex < Document.Scenes.Num(); ++SceneIndex)
    {
        const FAkUGCSceneDocument& Scene = Document.Scenes[SceneIndex];
        const FString ScenePath = FString::Printf(TEXT("scenes[%d]"), SceneIndex);

        if (!Scene.SceneId.IsValid())
        {
            Result.AddError(ScenePath + TEXT(".sceneId"), TEXT("Scene ID must be a valid GUID."));
        }
        else if (SceneIds.Contains(Scene.SceneId))
        {
            Result.AddError(ScenePath + TEXT(".sceneId"), TEXT("Scene ID must be unique."));
        }
        else
        {
            SceneIds.Add(Scene.SceneId);
        }

        TSet<FGuid> SceneEntityIds;
        TMap<FGuid, FGuid> ParentByEntity;
        TMap<FGuid, int32> EntityIndexById;
        TMap<FGuid, FName> PrefabIdByEntityId;

        for (int32 EntityIndex = 0; EntityIndex < Scene.Entities.Num(); ++EntityIndex)
        {
            const FAkUGCEntityRecord& Entity = Scene.Entities[EntityIndex];
            const FString EntityPath = FString::Printf(TEXT("%s.entities[%d]"), *ScenePath, EntityIndex);

            if (!Entity.EntityId.IsValid())
            {
                Result.AddError(EntityPath + TEXT(".entityId"), TEXT("Entity ID must be a valid GUID."));
            }
            else if (EntityIds.Contains(Entity.EntityId))
            {
                Result.AddError(EntityPath + TEXT(".entityId"), TEXT("Entity ID must be globally unique in the project."));
            }
            else
            {
                EntityIds.Add(Entity.EntityId);
                SceneEntityIds.Add(Entity.EntityId);
                EntityIndexById.Add(Entity.EntityId, EntityIndex);
                ParentByEntity.Add(Entity.EntityId, Entity.ParentEntityId);
                PrefabIdByEntityId.Add(Entity.EntityId, Entity.PrefabId);
            }

            if (Entity.PrefabId.IsNone())
            {
                Result.AddError(EntityPath + TEXT(".prefabId"), TEXT("Prefab ID is required."));
            }
            if (Entity.Transform.ContainsNaN())
            {
                Result.AddError(EntityPath + TEXT(".transform"), TEXT("Entity transform must be finite."));
            }

            for (int32 ComponentIndex = 0; ComponentIndex < Entity.Components.Num(); ++ComponentIndex)
            {
                const FAkUGCComponentRecord& Component = Entity.Components[ComponentIndex];
                const FString ComponentPath = FString::Printf(
                    TEXT("%s.components[%d]"),
                    *EntityPath,
                    ComponentIndex);

                if (Component.TypeId.IsNone())
                {
                    Result.AddError(ComponentPath + TEXT(".typeId"), TEXT("Component type ID is required."));
                }
                if (Component.SchemaVersion < 1)
                {
                    Result.AddError(ComponentPath + TEXT(".schemaVersion"), TEXT("Component schema version must be positive."));
                }

                for (const TPair<FName, FAkUGCValue>& Property : Component.Properties)
                {
                    const FString PropertyPath = ComponentPath + TEXT(".properties.") + Property.Key.ToString();
                    if (!FMath::IsFinite(Property.Value.NumberValue))
                    {
                        Result.AddError(PropertyPath + TEXT(".numberValue"), TEXT("Number field must be finite."));
                    }
                    if (Property.Value.VectorValue.ContainsNaN())
                    {
                        Result.AddError(PropertyPath + TEXT(".vectorValue"), TEXT("Vector field must be finite."));
                    }
                    if (Property.Value.RotatorValue.ContainsNaN())
                    {
                        Result.AddError(PropertyPath + TEXT(".rotatorValue"), TEXT("Rotator field must be finite."));
                    }
                }
            }
        }

        const FString RulesetPath = ScenePath + TEXT(".ruleset");
        if (Scene.Ruleset.Waves.Num() > AkUGCTowerDefenseRulesetLimits::RequiredWaveCount)
        {
            Result.AddError(
                RulesetPath + TEXT(".waves"),
                FString::Printf(
                    TEXT("Tower defense ruleset supports at most %d waves."),
                    AkUGCTowerDefenseRulesetLimits::RequiredWaveCount));
        }
        if (!FMath::IsFinite(Scene.Ruleset.WaveIntervalSeconds)
            || Scene.Ruleset.WaveIntervalSeconds < 0.0
            || Scene.Ruleset.WaveIntervalSeconds > AkUGCTowerDefenseRulesetLimits::MaxWaveIntervalSeconds)
        {
            Result.AddError(
                RulesetPath + TEXT(".waveIntervalSeconds"),
                TEXT("Wave interval must be finite and from 0 to 3600 seconds."));
        }
        switch (Scene.Ruleset.DefeatCondition)
        {
        case EAkUGCTowerDefenseDefeatCondition::BaseHealthDepleted:
            break;
        default:
            Result.AddError(RulesetPath + TEXT(".defeatCondition"), TEXT("Defeat condition is not supported."));
            break;
        }
        switch (Scene.Ruleset.VictoryCondition)
        {
        case EAkUGCTowerDefenseVictoryCondition::AllWavesCleared:
            break;
        default:
            Result.AddError(RulesetPath + TEXT(".victoryCondition"), TEXT("Victory condition is not supported."));
            break;
        }
        TSet<FGuid> WaveIds;
        for (int32 WaveIndex = 0; WaveIndex < Scene.Ruleset.Waves.Num(); ++WaveIndex)
        {
            const FAkUGCTowerDefenseWave& Wave = Scene.Ruleset.Waves[WaveIndex];
            const FString WavePath = FString::Printf(TEXT("%s.waves[%d]"), *RulesetPath, WaveIndex);
            if (!Wave.WaveId.IsValid())
            {
                Result.AddError(WavePath + TEXT(".waveId"), TEXT("Wave ID must be a valid GUID."));
            }
            else if (WaveIds.Contains(Wave.WaveId))
            {
                Result.AddError(WavePath + TEXT(".waveId"), TEXT("Wave ID must be unique in the Ruleset."));
            }
            else
            {
                WaveIds.Add(Wave.WaveId);
            }
            if (!Wave.SpawnPointEntityId.IsValid())
            {
                Result.AddError(WavePath + TEXT(".spawnPointEntityId"), TEXT("Wave Spawn Point ID must be a valid GUID."));
            }
            else if (!SceneEntityIds.Contains(Wave.SpawnPointEntityId))
            {
                Result.AddError(
                    WavePath + TEXT(".spawnPointEntityId"),
                    TEXT("Wave Spawn Point must exist in the same scene."));
            }
            else if (PrefabIdByEntityId.FindRef(Wave.SpawnPointEntityId) != TEXT("official.gameplay.enemy_spawn"))
            {
                Result.AddError(
                    WavePath + TEXT(".spawnPointEntityId"),
                    TEXT("Wave Spawn Point must reference official.gameplay.enemy_spawn."));
            }
            if (!FMath::IsFinite(Wave.StartDelaySeconds)
                || Wave.StartDelaySeconds < 0.0
                || Wave.StartDelaySeconds > AkUGCTowerDefenseRulesetLimits::MaxStartDelaySeconds)
            {
                Result.AddError(
                    WavePath + TEXT(".startDelaySeconds"),
                    TEXT("Wave start delay must be finite and from 0 to 3600 seconds."));
            }
        }

        for (const TPair<FGuid, FGuid>& Pair : ParentByEntity)
        {
            if (Pair.Value.IsValid() && !SceneEntityIds.Contains(Pair.Value))
            {
                const int32 EntityIndex = EntityIndexById.FindRef(Pair.Key);
                Result.AddError(
                    FString::Printf(TEXT("%s.entities[%d].parentEntityId"), *ScenePath, EntityIndex),
                    TEXT("Parent entity must exist in the same scene."));
            }
        }

        TMap<FGuid, uint8> ParentVisitStates;
        for (const TPair<FGuid, FGuid>& Pair : ParentByEntity)
        {
            if (HasParentCycle(Pair.Key, ParentByEntity, ParentVisitStates))
            {
                const int32 EntityIndex = EntityIndexById.FindRef(Pair.Key);
                Result.AddError(
                    FString::Printf(TEXT("%s.entities[%d].parentEntityId"), *ScenePath, EntityIndex),
                    TEXT("Parent hierarchy contains a cycle."));
                break;
            }
        }

        const FAkUGCValidationResult LogicValidation = ValidateLogicGraph(
            Scene.LogicGraph,
            ScenePath + TEXT(".logicGraph"));
        Result.Issues.Append(LogicValidation.Issues);
        for (int32 NodeIndex = 0; NodeIndex < Scene.LogicGraph.Nodes.Num(); ++NodeIndex)
        {
            const FAkUGCLogicNode& Node = Scene.LogicGraph.Nodes[NodeIndex];
            if (Node.Type == EAkUGCLogicNodeType::Spawn
                && Node.SpawnAtEntityId.IsValid()
                && !SceneEntityIds.Contains(Node.SpawnAtEntityId))
            {
                Result.AddError(
                    FString::Printf(TEXT("%s.logicGraph.nodes[%d].spawnAtEntityId"), *ScenePath, NodeIndex),
                    TEXT("Spawn anchor entity must exist in the same scene."));
            }
        }
    }

    if (Document.Scenes.IsEmpty())
    {
        Result.AddWarning(TEXT("scenes"), TEXT("Project does not contain a scene."));
    }

    return Result;
}

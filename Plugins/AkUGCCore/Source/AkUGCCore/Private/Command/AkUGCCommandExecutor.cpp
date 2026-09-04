#include "Command/AkUGCCommandExecutor.h"

#include "Algo/Reverse.h"
#include "Document/AkUGCDocument.h"
#include "Validation/AkUGCDocumentValidator.h"

namespace
{
    FAkUGCSceneDocument* FindScene(FAkUGCProjectDocument& Document, const FGuid& SceneId)
    {
        return Document.Scenes.FindByPredicate([&SceneId](const FAkUGCSceneDocument& Scene)
        {
            return Scene.SceneId == SceneId;
        });
    }

    FAkUGCEntityRecord* FindEntityInScene(FAkUGCSceneDocument& Scene, const FGuid& EntityId)
    {
        return Scene.Entities.FindByPredicate([&EntityId](const FAkUGCEntityRecord& Entity)
        {
            return Entity.EntityId == EntityId;
        });
    }

    const FAkUGCEntityRecord* FindEntityInScene(const FAkUGCSceneDocument& Scene, const FGuid& EntityId)
    {
        return Scene.Entities.FindByPredicate([&EntityId](const FAkUGCEntityRecord& Entity)
        {
            return Entity.EntityId == EntityId;
        });
    }

    bool ContainsEntity(const FAkUGCProjectDocument& Document, const FGuid& EntityId)
    {
        for (const FAkUGCSceneDocument& Scene : Document.Scenes)
        {
            if (FindEntityInScene(Scene, EntityId))
            {
                return true;
            }
        }
        return false;
    }

    FAkUGCCommand MakeInverse(const FAkUGCCommand& Source, EAkUGCCommandType Type)
    {
        FAkUGCCommand Inverse;
        Inverse.CommandId = FGuid::NewGuid();
        Inverse.AuthorId = Source.AuthorId;
        Inverse.Sequence = Source.Sequence;
        Inverse.Type = Type;
        Inverse.SceneId = Source.SceneId;
        Inverse.EntityId = Source.EntityId;
        return Inverse;
    }

    int32 FindLogicNodeIndex(const FAkUGCLogicGraph& LogicGraph, const FGuid& NodeId)
    {
        return LogicGraph.Nodes.IndexOfByPredicate([&NodeId](const FAkUGCLogicNode& Node)
        {
            return Node.NodeId == NodeId;
        });
    }

    bool LogicConnectionEquals(
        const FAkUGCLogicConnection& Left,
        const FAkUGCLogicConnection& Right)
    {
        return Left.SourceNodeId == Right.SourceNodeId
            && Left.TargetNodeId == Right.TargetNodeId;
    }

    FString CommandPath(int32 CommandIndex)
    {
        return FString::Printf(TEXT("commands[%d]"), CommandIndex);
    }
}

FAkUGCCommandExecutionResult FAkUGCCommandExecutionResult::Success()
{
    FAkUGCCommandExecutionResult Result;
    Result.bSucceeded = true;
    return Result;
}

FAkUGCCommandExecutionResult FAkUGCCommandExecutionResult::Failure(FString Path, FString Message)
{
    FAkUGCCommandExecutionResult Result;
    Result.ErrorPath = MoveTemp(Path);
    Result.ErrorMessage = MoveTemp(Message);
    return Result;
}

FAkUGCCommandExecutionResult FAkUGCCommandExecutor::Apply(
    FAkUGCProjectDocument& Document,
    const FAkUGCCommandTransaction& Transaction,
    FAkUGCCommandTransaction* OutInverse)
{
    if (!Transaction.TransactionId.IsValid())
    {
        return FAkUGCCommandExecutionResult::Failure(TEXT("transactionId"), TEXT("Transaction ID must be a valid GUID."));
    }
    if (Transaction.Commands.IsEmpty())
    {
        return FAkUGCCommandExecutionResult::Failure(TEXT("commands"), TEXT("Transaction must contain at least one command."));
    }

    FAkUGCProjectDocument WorkingDocument = Document;
    TArray<FAkUGCCommand> InverseCommands;
    InverseCommands.Reserve(Transaction.Commands.Num());

    TSet<FGuid> CommandIds;
    for (int32 CommandIndex = 0; CommandIndex < Transaction.Commands.Num(); ++CommandIndex)
    {
        const FAkUGCCommand& Command = Transaction.Commands[CommandIndex];
        if (!Command.CommandId.IsValid())
        {
            return FAkUGCCommandExecutionResult::Failure(
                CommandPath(CommandIndex) + TEXT(".commandId"),
                TEXT("Command ID must be a valid GUID."));
        }
        if (CommandIds.Contains(Command.CommandId))
        {
            return FAkUGCCommandExecutionResult::Failure(
                CommandPath(CommandIndex) + TEXT(".commandId"),
                TEXT("Command ID must be unique inside the transaction."));
        }
        CommandIds.Add(Command.CommandId);

        FAkUGCCommand Inverse;
        FAkUGCCommandExecutionResult Result = ApplySingle(WorkingDocument, Command, Inverse);
        if (!Result.bSucceeded)
        {
            Result.ErrorPath = CommandPath(CommandIndex) + TEXT(".") + Result.ErrorPath;
            return Result;
        }
        InverseCommands.Add(MoveTemp(Inverse));
    }

    const FAkUGCValidationResult Validation = FAkUGCDocumentValidator::Validate(WorkingDocument);
    if (!Validation.IsValid())
    {
        const FAkUGCValidationIssue* FirstError = Validation.Issues.FindByPredicate([](const FAkUGCValidationIssue& Issue)
        {
            return Issue.Severity == EAkUGCValidationSeverity::Error;
        });
        return FAkUGCCommandExecutionResult::Failure(
            FirstError ? TEXT("document.") + FirstError->Path : TEXT("document"),
            FirstError ? FirstError->Message : TEXT("Document validation failed."));
    }

    Document = MoveTemp(WorkingDocument);

    if (OutInverse)
    {
        Algo::Reverse(InverseCommands);
        OutInverse->TransactionId = FGuid::NewGuid();
        OutInverse->Label = FString::Printf(TEXT("Undo %s"), *Transaction.Label);
        OutInverse->Commands = MoveTemp(InverseCommands);
    }

    return FAkUGCCommandExecutionResult::Success();
}

FAkUGCCommandExecutionResult FAkUGCCommandExecutor::ApplySingle(
    FAkUGCProjectDocument& Document,
    const FAkUGCCommand& Command,
    FAkUGCCommand& OutInverse)
{
    FAkUGCSceneDocument* Scene = FindScene(Document, Command.SceneId);
    if (!Scene)
    {
        return FAkUGCCommandExecutionResult::Failure(TEXT("sceneId"), TEXT("Target scene does not exist."));
    }

    switch (Command.Type)
    {
    case EAkUGCCommandType::AddEntity:
    {
        const FAkUGCEntityRecord& Entity = Command.Entity;
        if (!Entity.EntityId.IsValid())
        {
            return FAkUGCCommandExecutionResult::Failure(TEXT("entity.entityId"), TEXT("Entity ID must be a valid GUID."));
        }
        if (Command.EntityId.IsValid() && Command.EntityId != Entity.EntityId)
        {
            return FAkUGCCommandExecutionResult::Failure(TEXT("entityId"), TEXT("Command entity ID does not match payload entity ID."));
        }
        if (ContainsEntity(Document, Entity.EntityId))
        {
            return FAkUGCCommandExecutionResult::Failure(TEXT("entity.entityId"), TEXT("Entity ID already exists in the project."));
        }
        if (Entity.PrefabId.IsNone())
        {
            return FAkUGCCommandExecutionResult::Failure(TEXT("entity.prefabId"), TEXT("Prefab ID is required."));
        }

        Scene->Entities.Add(Entity);
        OutInverse = MakeInverse(Command, EAkUGCCommandType::DeleteEntity);
        OutInverse.EntityId = Entity.EntityId;
        return FAkUGCCommandExecutionResult::Success();
    }

    case EAkUGCCommandType::DeleteEntity:
    {
        const int32 EntityIndex = Scene->Entities.IndexOfByPredicate([&Command](const FAkUGCEntityRecord& Entity)
        {
            return Entity.EntityId == Command.EntityId;
        });
        if (EntityIndex == INDEX_NONE)
        {
            return FAkUGCCommandExecutionResult::Failure(TEXT("entityId"), TEXT("Entity does not exist in the target scene."));
        }

        OutInverse = MakeInverse(Command, EAkUGCCommandType::AddEntity);
        OutInverse.EntityId = Command.EntityId;
        OutInverse.Entity = Scene->Entities[EntityIndex];
        Scene->Entities.RemoveAt(EntityIndex);
        return FAkUGCCommandExecutionResult::Success();
    }

    case EAkUGCCommandType::SetTransform:
    {
        FAkUGCEntityRecord* Entity = FindEntityInScene(*Scene, Command.EntityId);
        if (!Entity)
        {
            return FAkUGCCommandExecutionResult::Failure(TEXT("entityId"), TEXT("Entity does not exist in the target scene."));
        }
        if (Command.Transform.ContainsNaN())
        {
            return FAkUGCCommandExecutionResult::Failure(TEXT("transform"), TEXT("Transform contains a non-finite value."));
        }

        OutInverse = MakeInverse(Command, EAkUGCCommandType::SetTransform);
        OutInverse.Transform = Entity->Transform;
        Entity->Transform = Command.Transform;
        return FAkUGCCommandExecutionResult::Success();
    }

    case EAkUGCCommandType::SetProperty:
    case EAkUGCCommandType::RemoveProperty:
    {
        FAkUGCEntityRecord* Entity = FindEntityInScene(*Scene, Command.EntityId);
        if (!Entity)
        {
            return FAkUGCCommandExecutionResult::Failure(TEXT("entityId"), TEXT("Entity does not exist in the target scene."));
        }
        if (Command.ComponentTypeId.IsNone())
        {
            return FAkUGCCommandExecutionResult::Failure(TEXT("componentTypeId"), TEXT("Component type ID is required."));
        }
        if (Command.PropertyId.IsNone())
        {
            return FAkUGCCommandExecutionResult::Failure(TEXT("propertyId"), TEXT("Property ID is required."));
        }

        FAkUGCComponentRecord* Component = Entity->Components.FindByPredicate([&Command](const FAkUGCComponentRecord& Candidate)
        {
            return Candidate.TypeId == Command.ComponentTypeId;
        });
        if (!Component)
        {
            return FAkUGCCommandExecutionResult::Failure(TEXT("componentTypeId"), TEXT("Component does not exist on the entity."));
        }

        FAkUGCValue* ExistingValue = Component->Properties.Find(Command.PropertyId);
        if (Command.Type == EAkUGCCommandType::SetProperty)
        {
            OutInverse = MakeInverse(
                Command,
                ExistingValue ? EAkUGCCommandType::SetProperty : EAkUGCCommandType::RemoveProperty);
            OutInverse.ComponentTypeId = Command.ComponentTypeId;
            OutInverse.PropertyId = Command.PropertyId;
            if (ExistingValue)
            {
                OutInverse.PropertyValue = *ExistingValue;
            }
            Component->Properties.Add(Command.PropertyId, Command.PropertyValue);
            return FAkUGCCommandExecutionResult::Success();
        }

        if (!ExistingValue)
        {
            return FAkUGCCommandExecutionResult::Failure(TEXT("propertyId"), TEXT("Property does not exist on the component."));
        }
        OutInverse = MakeInverse(Command, EAkUGCCommandType::SetProperty);
        OutInverse.ComponentTypeId = Command.ComponentTypeId;
        OutInverse.PropertyId = Command.PropertyId;
        OutInverse.PropertyValue = *ExistingValue;
        Component->Properties.Remove(Command.PropertyId);
        return FAkUGCCommandExecutionResult::Success();
    }

    case EAkUGCCommandType::SetParent:
    {
        FAkUGCEntityRecord* Entity = FindEntityInScene(*Scene, Command.EntityId);
        if (!Entity)
        {
            return FAkUGCCommandExecutionResult::Failure(TEXT("entityId"), TEXT("Entity does not exist in the target scene."));
        }
        if (Command.ParentEntityId == Command.EntityId)
        {
            return FAkUGCCommandExecutionResult::Failure(TEXT("parentEntityId"), TEXT("Entity cannot be parented to itself."));
        }
        if (Command.ParentEntityId.IsValid() && !FindEntityInScene(*Scene, Command.ParentEntityId))
        {
            return FAkUGCCommandExecutionResult::Failure(
                TEXT("parentEntityId"),
                TEXT("Parent entity does not exist in the target scene."));
        }

        FGuid AncestorId = Command.ParentEntityId;
        while (AncestorId.IsValid())
        {
            if (AncestorId == Command.EntityId)
            {
                return FAkUGCCommandExecutionResult::Failure(
                    TEXT("parentEntityId"),
                    TEXT("Parent change would create a hierarchy cycle."));
            }
            const FAkUGCEntityRecord* Ancestor = FindEntityInScene(*Scene, AncestorId);
            AncestorId = Ancestor ? Ancestor->ParentEntityId : FGuid{};
        }

        OutInverse = MakeInverse(Command, EAkUGCCommandType::SetParent);
        OutInverse.ParentEntityId = Entity->ParentEntityId;
        Entity->ParentEntityId = Command.ParentEntityId;
        return FAkUGCCommandExecutionResult::Success();
    }

    case EAkUGCCommandType::DuplicateEntity:
    {
        const FAkUGCEntityRecord* Source = FindEntityInScene(*Scene, Command.SourceEntityId);
        if (!Source)
        {
            return FAkUGCCommandExecutionResult::Failure(TEXT("sourceEntityId"), TEXT("Source entity does not exist in the target scene."));
        }
        if (!Command.EntityId.IsValid())
        {
            return FAkUGCCommandExecutionResult::Failure(TEXT("entityId"), TEXT("New entity ID must be a valid GUID."));
        }
        if (ContainsEntity(Document, Command.EntityId))
        {
            return FAkUGCCommandExecutionResult::Failure(TEXT("entityId"), TEXT("New entity ID already exists in the project."));
        }

        FAkUGCEntityRecord Duplicate = *Source;
        Duplicate.EntityId = Command.EntityId;
        Duplicate.Transform = Command.Transform;
        Scene->Entities.Add(MoveTemp(Duplicate));

        OutInverse = MakeInverse(Command, EAkUGCCommandType::DeleteEntity);
        OutInverse.EntityId = Command.EntityId;
        return FAkUGCCommandExecutionResult::Success();
    }

    case EAkUGCCommandType::AddLogicNode:
    {
        if (!Command.LogicNode.NodeId.IsValid())
        {
            return FAkUGCCommandExecutionResult::Failure(TEXT("logicNode.nodeId"), TEXT("Logic node ID must be a valid GUID."));
        }
        if (FindLogicNodeIndex(Scene->LogicGraph, Command.LogicNode.NodeId) != INDEX_NONE)
        {
            return FAkUGCCommandExecutionResult::Failure(TEXT("logicNode.nodeId"), TEXT("Logic node ID already exists in the graph."));
        }
        Scene->LogicGraph.Nodes.Add(Command.LogicNode);
        for (const FAkUGCLogicConnection& Connection : Command.LogicConnections)
        {
            Scene->LogicGraph.Connections.Add(Connection);
        }

        OutInverse = MakeInverse(Command, EAkUGCCommandType::DeleteLogicNode);
        OutInverse.LogicNode = Command.LogicNode;
        return FAkUGCCommandExecutionResult::Success();
    }

    case EAkUGCCommandType::DeleteLogicNode:
    {
        const int32 NodeIndex = FindLogicNodeIndex(Scene->LogicGraph, Command.LogicNode.NodeId);
        if (NodeIndex == INDEX_NONE)
        {
            return FAkUGCCommandExecutionResult::Failure(TEXT("logicNode.nodeId"), TEXT("Logic node does not exist in the graph."));
        }

        OutInverse = MakeInverse(Command, EAkUGCCommandType::AddLogicNode);
        OutInverse.LogicNode = Scene->LogicGraph.Nodes[NodeIndex];
        OutInverse.LogicConnections = Scene->LogicGraph.Connections.FilterByPredicate([&Command](const FAkUGCLogicConnection& Connection)
        {
            return Connection.SourceNodeId == Command.LogicNode.NodeId
                || Connection.TargetNodeId == Command.LogicNode.NodeId;
        });
        Scene->LogicGraph.Connections.RemoveAll([&Command](const FAkUGCLogicConnection& Connection)
        {
            return Connection.SourceNodeId == Command.LogicNode.NodeId
                || Connection.TargetNodeId == Command.LogicNode.NodeId;
        });
        Scene->LogicGraph.Nodes.RemoveAt(NodeIndex);
        return FAkUGCCommandExecutionResult::Success();
    }

    case EAkUGCCommandType::ConnectLogicNode:
    {
        if (Scene->LogicGraph.Connections.ContainsByPredicate([&Command](const FAkUGCLogicConnection& Connection)
        {
            return LogicConnectionEquals(Connection, Command.LogicConnection);
        }))
        {
            return FAkUGCCommandExecutionResult::Failure(TEXT("logicConnection"), TEXT("Logic connection already exists in the graph."));
        }
        Scene->LogicGraph.Connections.Add(Command.LogicConnection);
        OutInverse = MakeInverse(Command, EAkUGCCommandType::DisconnectLogicNode);
        OutInverse.LogicConnection = Command.LogicConnection;
        return FAkUGCCommandExecutionResult::Success();
    }

    case EAkUGCCommandType::DisconnectLogicNode:
    {
        const int32 ConnectionIndex = Scene->LogicGraph.Connections.IndexOfByPredicate(
            [&Command](const FAkUGCLogicConnection& Connection)
            {
                return LogicConnectionEquals(Connection, Command.LogicConnection);
            });
        if (ConnectionIndex == INDEX_NONE)
        {
            return FAkUGCCommandExecutionResult::Failure(TEXT("logicConnection"), TEXT("Logic connection does not exist in the graph."));
        }
        Scene->LogicGraph.Connections.RemoveAt(ConnectionIndex);
        OutInverse = MakeInverse(Command, EAkUGCCommandType::ConnectLogicNode);
        OutInverse.LogicConnection = Command.LogicConnection;
        return FAkUGCCommandExecutionResult::Success();
    }
    }

    return FAkUGCCommandExecutionResult::Failure(TEXT("type"), TEXT("Unsupported command type."));
}

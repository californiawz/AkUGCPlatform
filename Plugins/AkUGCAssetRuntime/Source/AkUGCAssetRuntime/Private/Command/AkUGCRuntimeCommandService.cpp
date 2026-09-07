#include "Command/AkUGCRuntimeCommandService.h"

#include "Command/AkUGCCommand.h"
#include "Prefab/AkUGCPrefabRegistry.h"
#include "Session/AkUGCDocumentRuntimeSession.h"

namespace
{
    FAkUGCCommandExecutionResult Failure(FString Path, FString Message)
    {
        return FAkUGCCommandExecutionResult::Failure(MoveTemp(Path), MoveTemp(Message));
    }

    bool GuidLess(const FGuid& Left, const FGuid& Right)
    {
        return Left.ToString(EGuidFormats::Digits) < Right.ToString(EGuidFormats::Digits);
    }

    bool IsIntegerWithinRange(int64 Value, const FAkUGCPropertyDefinition& Property)
    {
        constexpr double Int64ExclusiveUpper = 9223372036854775808.0;
        constexpr double Int64Lower = -9223372036854775808.0;
        if (Property.bHasMinimum)
        {
            if (Property.Minimum >= Int64ExclusiveUpper)
            {
                return false;
            }
            if (Property.Minimum > Int64Lower)
            {
                const int64 MinimumInteger = static_cast<int64>(FMath::CeilToDouble(Property.Minimum));
                if (Value < MinimumInteger)
                {
                    return false;
                }
            }
        }
        if (Property.bHasMaximum)
        {
            if (Property.Maximum < Int64Lower)
            {
                return false;
            }
            if (Property.Maximum < Int64ExclusiveUpper)
            {
                const int64 MaximumInteger = static_cast<int64>(FMath::FloorToDouble(Property.Maximum));
                if (Value > MaximumInteger)
                {
                    return false;
                }
            }
        }
        return true;
    }

    FAkUGCValue* FindPathOrder(FAkUGCEntityRecord& Entity)
    {
        FAkUGCComponentRecord* Component = Entity.Components.FindByPredicate([](const FAkUGCComponentRecord& Candidate)
        {
            return Candidate.TypeId == TEXT("tower_defense.path_node");
        });
        return Component ? Component->Properties.Find(TEXT("order")) : nullptr;
    }

    bool AssignNextPathOrder(
        FAkUGCEntityRecord& Entity,
        const FAkUGCSceneDocument& Scene,
        FString& OutError)
    {
        if (Entity.PrefabId != TEXT("official.gameplay.path_node"))
        {
            return true;
        }

        FAkUGCValue* NewOrder = FindPathOrder(Entity);
        if (!NewOrder || NewOrder->Type != EAkUGCValueType::Integer)
        {
            OutError = TEXT("Path node prefab does not provide an Integer order property.");
            return false;
        }

        TSet<int64> UsedOrders;
        for (const FAkUGCEntityRecord& Existing : Scene.Entities)
        {
            if (Existing.PrefabId != TEXT("official.gameplay.path_node"))
            {
                continue;
            }
            const FAkUGCComponentRecord* Component = Existing.Components.FindByPredicate([](const FAkUGCComponentRecord& Candidate)
            {
                return Candidate.TypeId == TEXT("tower_defense.path_node");
            });
            const FAkUGCValue* Order = Component ? Component->Properties.Find(TEXT("order")) : nullptr;
            if (Order && Order->Type == EAkUGCValueType::Integer)
            {
                UsedOrders.Add(Order->IntegerValue);
            }
        }

        for (int64 CandidateOrder = 0; CandidateOrder <= 10000; ++CandidateOrder)
        {
            if (!UsedOrders.Contains(CandidateOrder))
            {
                NewOrder->IntegerValue = CandidateOrder;
                return true;
            }
        }
        OutError = TEXT("No free path node order remains in the supported range 0 to 10000.");
        return false;
    }
}

FAkUGCRuntimeCommandService::FAkUGCRuntimeCommandService(
    FAkUGCProjectDocument& InDocument,
    FAkUGCDocumentRuntimeSession& InSession,
    const FAkUGCPrefabRegistry& InRegistry,
    FGuid InSceneId,
    EAkUGCEditingClient InEditingClient)
    : Document(InDocument)
    , Session(InSession)
    , Registry(InRegistry)
    , SceneId(InSceneId)
    , EditingClient(InEditingClient)
{
}

FAkUGCCommandExecutionResult FAkUGCRuntimeCommandService::PlacePrefab(
    FName PrefabId,
    const FTransform& Transform,
    FGuid& OutEntityId)
{
    OutEntityId = FGuid::NewGuid();
    FAkUGCEntityRecord Entity;
    FString Error;
    if (!Registry.CreateEntityRecord(PrefabId, OutEntityId, Transform, Entity, &Error))
    {
        OutEntityId.Invalidate();
        return Failure(TEXT("commandService.prefabId"), MoveTemp(Error));
    }
    const FAkUGCSceneDocument* Scene = FindScene();
    if (!Scene || !AssignNextPathOrder(Entity, *Scene, Error))
    {
        OutEntityId.Invalidate();
        return Failure(TEXT("commandService.pathOrder"),
            Scene ? MoveTemp(Error) : TEXT("Active scene does not exist."));
    }

    FAkUGCCommand Command;
    Command.CommandId = FGuid::NewGuid();
    Command.Type = EAkUGCCommandType::AddEntity;
    Command.SceneId = SceneId;
    Command.EntityId = OutEntityId;
    Command.Entity = MoveTemp(Entity);

    FAkUGCCommandTransaction Transaction;
    Transaction.TransactionId = FGuid::NewGuid();
    Transaction.Label = FString::Printf(TEXT("Place %s"), *PrefabId.ToString());
    Transaction.Commands.Add(MoveTemp(Command));
    FAkUGCCommandExecutionResult Result = Execute(MoveTemp(Transaction));
    if (!Result.bSucceeded)
    {
        OutEntityId.Invalidate();
    }
    return Result;
}

FAkUGCCommandExecutionResult FAkUGCRuntimeCommandService::DeleteEntities(
    const TSet<FGuid>& EntityIds,
    const FString& Label)
{
    if (EntityIds.IsEmpty())
    {
        return Failure(TEXT("commandService.entityIds"), TEXT("At least one entity is required."));
    }

    const FAkUGCSceneDocument* Scene = FindScene();
    if (!Scene)
    {
        return Failure(TEXT("commandService.sceneId"), TEXT("Active scene does not exist."));
    }

    TSet<FGuid> ExistingEntityIds;
    for (const FGuid& EntityId : EntityIds)
    {
        if (Scene->Entities.ContainsByPredicate([&EntityId](const FAkUGCEntityRecord& Entity)
        {
            return Entity.EntityId == EntityId;
        }))
        {
            ExistingEntityIds.Add(EntityId);
        }
    }
    if (ExistingEntityIds.IsEmpty())
    {
        return Failure(TEXT("commandService.entityIds"), TEXT("No target entity exists."));
    }

    FAkUGCCommandTransaction Transaction;
    Transaction.TransactionId = FGuid::NewGuid();
    Transaction.Label = Label;

    for (const FAkUGCEntityRecord& Entity : Scene->Entities)
    {
        if (Entity.ParentEntityId.IsValid()
            && ExistingEntityIds.Contains(Entity.ParentEntityId)
            && !ExistingEntityIds.Contains(Entity.EntityId))
        {
            FAkUGCCommand& DetachCommand = Transaction.Commands.AddDefaulted_GetRef();
            DetachCommand.CommandId = FGuid::NewGuid();
            DetachCommand.Type = EAkUGCCommandType::SetParent;
            DetachCommand.SceneId = SceneId;
            DetachCommand.EntityId = Entity.EntityId;
        }
    }

    const auto GetDepth = [Scene](const FGuid& EntityId)
    {
        int32 Depth = 0;
        const FAkUGCEntityRecord* Entity = Scene->Entities.FindByPredicate([&EntityId](const FAkUGCEntityRecord& Candidate)
        {
            return Candidate.EntityId == EntityId;
        });
        FGuid ParentId = Entity ? Entity->ParentEntityId : FGuid{};
        while (ParentId.IsValid())
        {
            ++Depth;
            const FAkUGCEntityRecord* Parent = Scene->Entities.FindByPredicate([&ParentId](const FAkUGCEntityRecord& Candidate)
            {
                return Candidate.EntityId == ParentId;
            });
            ParentId = Parent ? Parent->ParentEntityId : FGuid{};
        }
        return Depth;
    };

    TArray<FGuid> OrderedEntityIds = ExistingEntityIds.Array();
    OrderedEntityIds.Sort([&GetDepth](const FGuid& Left, const FGuid& Right)
    {
        const int32 LeftDepth = GetDepth(Left);
        const int32 RightDepth = GetDepth(Right);
        return LeftDepth == RightDepth ? GuidLess(Left, Right) : LeftDepth > RightDepth;
    });

    for (const FGuid& EntityId : OrderedEntityIds)
    {
        FAkUGCCommand& DeleteCommand = Transaction.Commands.AddDefaulted_GetRef();
        DeleteCommand.CommandId = FGuid::NewGuid();
        DeleteCommand.Type = EAkUGCCommandType::DeleteEntity;
        DeleteCommand.SceneId = SceneId;
        DeleteCommand.EntityId = EntityId;
    }
    return Execute(MoveTemp(Transaction));
}

FAkUGCCommandExecutionResult FAkUGCRuntimeCommandService::DuplicateEntity(
    const FGuid& SourceEntityId,
    const FVector& WorldOffset,
    FGuid& OutEntityId)
{
    OutEntityId.Invalidate();
    const FAkUGCEntityRecord* Source = FindEntity(SourceEntityId);
    if (!Source)
    {
        OutEntityId.Invalidate();
        return Failure(TEXT("commandService.sourceEntityId"), TEXT("Source entity does not exist in the active scene."));
    }
    if (WorldOffset.ContainsNaN())
    {
        OutEntityId.Invalidate();
        return Failure(TEXT("commandService.worldOffset"), TEXT("Duplicate offset must be finite."));
    }

    OutEntityId = FGuid::NewGuid();
    FAkUGCCommand Command;
    Command.CommandId = FGuid::NewGuid();
    Command.Type = EAkUGCCommandType::DuplicateEntity;
    Command.SceneId = SceneId;
    Command.EntityId = OutEntityId;
    Command.SourceEntityId = SourceEntityId;
    Command.Transform = Source->Transform;
    Command.Transform.AddToTranslation(WorldOffset);

    TOptional<FAkUGCValue> PathOrder;
    if (Source->PrefabId == TEXT("official.gameplay.path_node"))
    {
        const FAkUGCSceneDocument* Scene = FindScene();
        FAkUGCEntityRecord Duplicate = *Source;
        Duplicate.EntityId = OutEntityId;
        Duplicate.Transform = Command.Transform;
        FString Error;
        if (!Scene || !AssignNextPathOrder(Duplicate, *Scene, Error))
        {
            OutEntityId.Invalidate();
            return Failure(TEXT("commandService.pathOrder"),
                Scene ? MoveTemp(Error) : TEXT("Active scene does not exist."));
        }
        PathOrder = *FindPathOrder(Duplicate);
    }

    FAkUGCCommandTransaction Transaction;
    Transaction.TransactionId = FGuid::NewGuid();
    Transaction.Label = TEXT("Duplicate entity");
    Transaction.Commands.Add(MoveTemp(Command));
    if (PathOrder.IsSet())
    {
        FAkUGCCommand& SetOrder = Transaction.Commands.AddDefaulted_GetRef();
        SetOrder.CommandId = FGuid::NewGuid();
        SetOrder.Type = EAkUGCCommandType::SetProperty;
        SetOrder.SceneId = SceneId;
        SetOrder.EntityId = OutEntityId;
        SetOrder.ComponentTypeId = TEXT("tower_defense.path_node");
        SetOrder.PropertyId = TEXT("order");
        SetOrder.PropertyValue = PathOrder.GetValue();
    }
    FAkUGCCommandExecutionResult Result = Execute(MoveTemp(Transaction));
    if (!Result.bSucceeded)
    {
        OutEntityId.Invalidate();
    }
    return Result;
}

FAkUGCCommandExecutionResult FAkUGCRuntimeCommandService::SetEntityTransform(
    const FGuid& EntityId,
    const FTransform& Transform)
{
    return SetEntityTransforms({{EntityId, Transform}}, TEXT("Move entity"));
}

FAkUGCCommandExecutionResult FAkUGCRuntimeCommandService::SetEntityTransforms(
    const TMap<FGuid, FTransform>& Transforms,
    const FString& Label)
{
    if (Transforms.IsEmpty())
    {
        return Failure(TEXT("commandService.transforms"), TEXT("At least one entity transform is required."));
    }

    TArray<FGuid> EntityIds;
    Transforms.GetKeys(EntityIds);
    EntityIds.Sort(GuidLess);

    FAkUGCCommandTransaction Transaction;
    Transaction.TransactionId = FGuid::NewGuid();
    Transaction.Label = Label;
    for (const FGuid& EntityId : EntityIds)
    {
        FAkUGCCommand& Command = Transaction.Commands.AddDefaulted_GetRef();
        Command.CommandId = FGuid::NewGuid();
        Command.Type = EAkUGCCommandType::SetTransform;
        Command.SceneId = SceneId;
        Command.EntityId = EntityId;
        Command.Transform = Transforms.FindChecked(EntityId);
    }
    return Execute(MoveTemp(Transaction));
}

FAkUGCCommandExecutionResult FAkUGCRuntimeCommandService::SetEntityParent(
    const FGuid& EntityId,
    const FGuid& ParentEntityId)
{
    return SetEntityParentAndTransform(
        EntityId,
        ParentEntityId,
        {},
        ParentEntityId.IsValid() ? TEXT("Set entity parent") : TEXT("Detach entity from parent"));
}

FAkUGCCommandExecutionResult FAkUGCRuntimeCommandService::SetEntityParentAndTransform(
    const FGuid& EntityId,
    const FGuid& ParentEntityId,
    const TOptional<FTransform>& WorldTransform,
    const FString& Label)
{
    const FAkUGCEntityRecord* Entity = FindEntity(EntityId);
    if (!Entity)
    {
        return Failure(TEXT("commandService.entityId"), TEXT("Entity does not exist in the active scene."));
    }

    FAkUGCCommandTransaction Transaction;
    Transaction.TransactionId = FGuid::NewGuid();
    Transaction.Label = Label;
    if (Entity->ParentEntityId != ParentEntityId)
    {
        FAkUGCCommand& ParentCommand = Transaction.Commands.AddDefaulted_GetRef();
        ParentCommand.CommandId = FGuid::NewGuid();
        ParentCommand.Type = EAkUGCCommandType::SetParent;
        ParentCommand.SceneId = SceneId;
        ParentCommand.EntityId = EntityId;
        ParentCommand.ParentEntityId = ParentEntityId;
    }
    if (WorldTransform.IsSet() && !Entity->Transform.Equals(WorldTransform.GetValue(), 0.01))
    {
        FAkUGCCommand& TransformCommand = Transaction.Commands.AddDefaulted_GetRef();
        TransformCommand.CommandId = FGuid::NewGuid();
        TransformCommand.Type = EAkUGCCommandType::SetTransform;
        TransformCommand.SceneId = SceneId;
        TransformCommand.EntityId = EntityId;
        TransformCommand.Transform = WorldTransform.GetValue();
    }
    if (Transaction.Commands.IsEmpty())
    {
        return FAkUGCCommandExecutionResult::Success();
    }
    return Execute(MoveTemp(Transaction));
}

FAkUGCCommandExecutionResult FAkUGCRuntimeCommandService::SetEntityProperty(
    const FGuid& EntityId,
    FName ComponentTypeId,
    FName PropertyId,
    const FAkUGCValue& Value)
{
    const FAkUGCEntityRecord* Entity = FindEntity(EntityId);
    const FAkUGCPrefabDefinition* Prefab = Entity ? Registry.Find(Entity->PrefabId) : nullptr;
    if (!Prefab)
    {
        return Failure(TEXT("commandService.entityId"), TEXT("Entity or prefab definition does not exist."));
    }

    const FAkUGCPropertyDefinition* Property = Prefab->EditableProperties.FindByPredicate(
        [ComponentTypeId, PropertyId](const FAkUGCPropertyDefinition& Candidate)
        {
            return Candidate.ComponentTypeId == ComponentTypeId && Candidate.PropertyId == PropertyId;
        });
    if (!Property)
    {
        return Failure(TEXT("commandService.propertyId"), TEXT("Property is not exposed by the prefab schema."));
    }
    if (EditingClient == EAkUGCEditingClient::Mobile && !Property->bMobileEditable)
    {
        return Failure(TEXT("commandService.propertyId"), TEXT("Property is not editable on mobile clients."));
    }
    if (Property->ValueType != Value.Type)
    {
        return Failure(TEXT("commandService.propertyValue"), TEXT("Property value type does not match the prefab schema."));
    }
    if (Value.Type == EAkUGCValueType::Number && !FMath::IsFinite(Value.NumberValue))
    {
        return Failure(TEXT("commandService.propertyValue"), TEXT("Numeric property value must be finite."));
    }
    if (Value.Type == EAkUGCValueType::Vector && Value.VectorValue.ContainsNaN())
    {
        return Failure(TEXT("commandService.propertyValue"), TEXT("Vector property value must be finite."));
    }
    if (Value.Type == EAkUGCValueType::Rotator && Value.RotatorValue.ContainsNaN())
    {
        return Failure(TEXT("commandService.propertyValue"), TEXT("Rotator property value must be finite."));
    }

    if (Value.Type == EAkUGCValueType::Integer)
    {
        if (!IsIntegerWithinRange(Value.IntegerValue, *Property))
        {
            return Failure(TEXT("commandService.propertyValue"), TEXT("Property value is outside the allowed range."));
        }
    }
    else if (Value.Type == EAkUGCValueType::Number
        && ((Property->bHasMinimum && Value.NumberValue < Property->Minimum)
            || (Property->bHasMaximum && Value.NumberValue > Property->Maximum)))
    {
        return Failure(TEXT("commandService.propertyValue"), TEXT("Property value is outside the allowed range."));
    }

    FAkUGCCommand Command;
    Command.CommandId = FGuid::NewGuid();
    Command.Type = EAkUGCCommandType::SetProperty;
    Command.SceneId = SceneId;
    Command.EntityId = EntityId;
    Command.ComponentTypeId = ComponentTypeId;
    Command.PropertyId = PropertyId;
    Command.PropertyValue = Value;

    FAkUGCCommandTransaction Transaction;
    Transaction.TransactionId = FGuid::NewGuid();
    Transaction.Label = FString::Printf(TEXT("Set %s.%s"), *ComponentTypeId.ToString(), *PropertyId.ToString());
    Transaction.Commands.Add(MoveTemp(Command));
    return Execute(MoveTemp(Transaction));
}

FAkUGCCommandExecutionResult FAkUGCRuntimeCommandService::Undo()
{
    return Session.Undo(Document);
}

FAkUGCCommandExecutionResult FAkUGCRuntimeCommandService::Redo()
{
    return Session.Redo(Document);
}

bool FAkUGCRuntimeCommandService::CanUndo() const
{
    return Session.CanUndo();
}

bool FAkUGCRuntimeCommandService::CanRedo() const
{
    return Session.CanRedo();
}

const FAkUGCEntityRecord* FAkUGCRuntimeCommandService::FindEntity(const FGuid& EntityId) const
{
    const FAkUGCSceneDocument* Scene = FindScene();
    return Scene ? Scene->Entities.FindByPredicate([&EntityId](const FAkUGCEntityRecord& Entity)
    {
        return Entity.EntityId == EntityId;
    }) : nullptr;
}

const FAkUGCSceneDocument* FAkUGCRuntimeCommandService::FindScene() const
{
    return Document.Scenes.FindByPredicate([this](const FAkUGCSceneDocument& Scene)
    {
        return Scene.SceneId == SceneId;
    });
}

FAkUGCCommandExecutionResult FAkUGCRuntimeCommandService::Execute(FAkUGCCommandTransaction&& Transaction)
{
    return Session.Execute(Document, Transaction);
}

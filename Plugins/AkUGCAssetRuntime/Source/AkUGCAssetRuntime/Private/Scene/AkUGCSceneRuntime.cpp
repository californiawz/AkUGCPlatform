#include "Scene/AkUGCSceneRuntime.h"

#include "Command/AkUGCCommand.h"
#include "Components/StaticMeshComponent.h"
#include "Document/AkUGCDocument.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Entity/AkUGCEntityBindingComponent.h"
#include "Entity/AkUGCRuntimeEntityActor.h"
#include "Prefab/AkUGCPrefabRegistry.h"
#include "Subsystem/AkUGCLogicRuntimeSubsystem.h"

namespace
{
    bool Fail(FString* OutError, const FString& Message)
    {
        if (OutError)
        {
            *OutError = Message;
        }
        return false;
    }

    UAkUGCEntityBindingComponent* FindBinding(AActor* Actor)
    {
        return Actor ? Actor->FindComponentByClass<UAkUGCEntityBindingComponent>() : nullptr;
    }
}

FAkUGCSceneRuntime::FAkUGCSceneRuntime(UWorld* InWorld)
    : World(InWorld)
{
}

FAkUGCSceneRuntime::~FAkUGCSceneRuntime()
{
    Unload();
}

bool FAkUGCSceneRuntime::LoadScene(
    const FAkUGCSceneDocument& Scene,
    const FAkUGCPrefabRegistry& Registry,
    FString* OutError)
{
    if (!ValidateScene(Scene, Registry, OutError))
    {
        return false;
    }

    Unload();
    ActiveSceneId = Scene.SceneId;

    for (const FAkUGCEntityRecord& Entity : Scene.Entities)
    {
        if (!SpawnEntity(Entity, Registry, OutError))
        {
            Unload();
            return false;
        }
    }

    if (!AttachParents(Scene, OutError))
    {
        Unload();
        return false;
    }

    return true;
}

bool FAkUGCSceneRuntime::RunGameStartLogic(
    const FAkUGCSceneDocument& Scene,
    FString* OutError)
{
    UWorld* RuntimeWorld = World.Get();
    if (RuntimeWorld
        && RuntimeWorld->WorldType != EWorldType::Game
        && RuntimeWorld->WorldType != EWorldType::PIE
        && RuntimeWorld->WorldType != EWorldType::GamePreview)
    {
        return true;
    }
    UAkUGCLogicRuntimeSubsystem* LogicRuntime = RuntimeWorld
        ? RuntimeWorld->GetSubsystem<UAkUGCLogicRuntimeSubsystem>()
        : nullptr;
    if (!LogicRuntime)
    {
        return Fail(OutError, TEXT("Logic Runtime subsystem is not available for the scene world."));
    }

    const FAkUGCLogicRuntimeResult Result = LogicRuntime->RunGameStart(Scene.LogicGraph);
    return Result.bSucceeded
        ? true
        : Fail(OutError, FString::Printf(TEXT("%s: %s"), *Result.ErrorPath, *Result.ErrorMessage));
}

bool FAkUGCSceneRuntime::SynchronizeScene(
    const FAkUGCSceneDocument& Scene,
    const FAkUGCPrefabRegistry& Registry,
    FString* OutError)
{
    if (!ValidateScene(Scene, Registry, OutError))
    {
        return false;
    }
    if (ActiveSceneId != Scene.SceneId)
    {
        return LoadScene(Scene, Registry, OutError);
    }

    TSet<FGuid> DesiredEntityIds;
    for (const FAkUGCEntityRecord& Entity : Scene.Entities)
    {
        DesiredEntityIds.Add(Entity.EntityId);
    }

    TArray<FGuid> ExistingEntityIds;
    Actors.GetKeys(ExistingEntityIds);
    for (const FGuid& ExistingEntityId : ExistingEntityIds)
    {
        if (!DesiredEntityIds.Contains(ExistingEntityId))
        {
            RemoveEntity(ExistingEntityId);
        }
    }

    for (const TPair<FGuid, TWeakObjectPtr<AActor>>& Pair : Actors)
    {
        if (AActor* Actor = Pair.Value.Get())
        {
            Actor->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
        }
    }

    for (const FAkUGCEntityRecord& Entity : Scene.Entities)
    {
        AActor* ExistingActor = FindActor(Entity.EntityId);
        const UAkUGCEntityBindingComponent* Binding = FindBinding(ExistingActor);
        if (Binding && Binding->PrefabId != Entity.PrefabId)
        {
            RemoveEntity(Entity.EntityId);
            ExistingActor = nullptr;
        }

        if (ExistingActor)
        {
            if (!ApplyEntity(Entity, Registry, OutError))
            {
                return false;
            }
        }
        else if (!SpawnEntity(Entity, Registry, OutError))
        {
            return false;
        }
    }

    return AttachParents(Scene, OutError);
}

bool FAkUGCSceneRuntime::ApplyTransaction(
    const FAkUGCCommandTransaction& Transaction,
    const FAkUGCPrefabRegistry& Registry,
    FString* OutError)
{
    if (!ActiveSceneId.IsValid())
    {
        return Fail(OutError, TEXT("Runtime scene is not initialized."));
    }

    TSet<FGuid> AttachmentUpdates;
    for (const FAkUGCCommand& Command : Transaction.Commands)
    {
        if (Command.SceneId != ActiveSceneId)
        {
            return Fail(OutError, TEXT("Command targets a different scene than the active runtime scene."));
        }
        if (Command.Type == EAkUGCCommandType::SetTransform
            || Command.Type == EAkUGCCommandType::SetParent)
        {
            if (AActor* Actor = FindActor(Command.EntityId))
            {
                if (Command.Type == EAkUGCCommandType::SetTransform)
                {
                    TArray<AActor*> Descendants;
                    Actor->GetAttachedActors(Descendants, false, true);
                    for (AActor* Descendant : Descendants)
                    {
                        if (const UAkUGCEntityBindingComponent* Binding = FindBinding(Descendant))
                        {
                            Descendant->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
                            AttachmentUpdates.Add(Binding->EntityId);
                        }
                    }
                }
                Actor->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
                AttachmentUpdates.Add(Command.EntityId);
            }
        }
    }

    for (const FAkUGCCommand& Command : Transaction.Commands)
    {
        if (!ApplyCommand(Command, Registry, AttachmentUpdates, OutError))
        {
            return false;
        }
    }

    return RefreshAttachments(AttachmentUpdates, OutError);
}

bool FAkUGCSceneRuntime::ApplyEntity(
    const FAkUGCEntityRecord& Entity,
    const FAkUGCPrefabRegistry& Registry,
    FString* OutError)
{
    AActor* Actor = FindActor(Entity.EntityId);
    if (!Actor)
    {
        return SpawnEntity(Entity, Registry, OutError);
    }

    UAkUGCEntityBindingComponent* Binding = FindBinding(Actor);
    if (!Binding)
    {
        return Fail(OutError, TEXT("Runtime actor does not contain a UGC binding component."));
    }
    if (Binding->PrefabId != Entity.PrefabId)
    {
        return Fail(OutError, TEXT("Changing PrefabId in place is not supported; remove and add the entity instead."));
    }

    Binding->ApplyRecord(Entity);
    return true;
}

bool FAkUGCSceneRuntime::RemoveEntity(const FGuid& EntityId)
{
    TWeakObjectPtr<AActor>* ActorPtr = Actors.Find(EntityId);
    if (!ActorPtr)
    {
        return false;
    }

    if (AActor* Actor = ActorPtr->Get())
    {
        TArray<AActor*> AttachedActors;
        Actor->GetAttachedActors(AttachedActors);
        for (AActor* AttachedActor : AttachedActors)
        {
            if (AttachedActor)
            {
                AttachedActor->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
            }
        }
        Actor->Destroy();
    }
    Actors.Remove(EntityId);
    ExternallyDeletedEntityIds.Remove(EntityId);
    return true;
}

bool FAkUGCSceneRuntime::NotifyActorDeletedExternally(const FGuid& EntityId, const AActor* Actor)
{
    const TWeakObjectPtr<AActor>* ExistingActor = Actors.Find(EntityId);
    if (!ExistingActor || ExistingActor->Get() != Actor)
    {
        return false;
    }

    Actors.Remove(EntityId);
    ExternallyDeletedEntityIds.Add(EntityId);
    return true;
}

void FAkUGCSceneRuntime::Unload()
{
    if (UWorld* RuntimeWorld = World.Get())
    {
        if (UAkUGCLogicRuntimeSubsystem* LogicRuntime = RuntimeWorld->GetSubsystem<UAkUGCLogicRuntimeSubsystem>())
        {
            LogicRuntime->ResetLogicRuntime();
        }
    }

    for (TPair<FGuid, TWeakObjectPtr<AActor>>& Pair : Actors)
    {
        if (AActor* Actor = Pair.Value.Get())
        {
            Actor->Destroy();
        }
    }
    Actors.Reset();
    ExternallyDeletedEntityIds.Reset();
    ActiveSceneId.Invalidate();
}

AActor* FAkUGCSceneRuntime::FindActor(const FGuid& EntityId) const
{
    const TWeakObjectPtr<AActor>* Actor = Actors.Find(EntityId);
    return Actor ? Actor->Get() : nullptr;
}

int32 FAkUGCSceneRuntime::Num() const
{
    return Actors.Num();
}

FGuid FAkUGCSceneRuntime::GetActiveSceneId() const
{
    return ActiveSceneId;
}

FName FAkUGCSceneRuntime::GetCurrentPlatformVariant()
{
#if UE_SERVER
    return TEXT("Server");
#elif PLATFORM_ANDROID
    return TEXT("Android");
#elif PLATFORM_IOS
    return TEXT("IOS");
#elif PLATFORM_WINDOWS
    return TEXT("Win64");
#else
    return TEXT("Default");
#endif
}

bool FAkUGCSceneRuntime::ValidateScene(
    const FAkUGCSceneDocument& Scene,
    const FAkUGCPrefabRegistry& Registry,
    FString* OutError) const
{
    if (!World.IsValid())
    {
        return Fail(OutError, TEXT("Runtime world is not valid."));
    }
    if (!Scene.SceneId.IsValid())
    {
        return Fail(OutError, TEXT("Scene ID must be a valid GUID."));
    }

    TSet<FGuid> EntityIds;
    TMap<FGuid, FGuid> ParentByEntity;
    for (const FAkUGCEntityRecord& Entity : Scene.Entities)
    {
        if (!Entity.EntityId.IsValid())
        {
            return Fail(OutError, TEXT("Scene contains an invalid entity ID."));
        }
        if (EntityIds.Contains(Entity.EntityId))
        {
            return Fail(OutError, TEXT("Scene contains a duplicate entity ID."));
        }
        if (!Registry.Find(Entity.PrefabId))
        {
            return Fail(OutError, FString::Printf(TEXT("Prefab '%s' is not registered."), *Entity.PrefabId.ToString()));
        }
        EntityIds.Add(Entity.EntityId);
        ParentByEntity.Add(Entity.EntityId, Entity.ParentEntityId);
    }

    for (const FAkUGCEntityRecord& Entity : Scene.Entities)
    {
        if (Entity.ParentEntityId.IsValid() && !EntityIds.Contains(Entity.ParentEntityId))
        {
            return Fail(OutError, FString::Printf(
                TEXT("Parent '%s' for entity '%s' is not part of the scene."),
                *Entity.ParentEntityId.ToString(),
                *Entity.EntityId.ToString()));
        }
        if (Entity.ParentEntityId == Entity.EntityId)
        {
            return Fail(OutError, TEXT("Entity cannot be parented to itself."));
        }
    }

    TMap<FGuid, uint8> VisitStates;
    TFunction<bool(const FGuid&)> VisitParent = [&](const FGuid& EntityId)
    {
        const uint8 State = VisitStates.FindRef(EntityId);
        if (State == 1)
        {
            return false;
        }
        if (State == 2)
        {
            return true;
        }

        VisitStates.Add(EntityId, 1);
        const FGuid ParentId = ParentByEntity.FindRef(EntityId);
        if (ParentId.IsValid() && !VisitParent(ParentId))
        {
            return false;
        }
        VisitStates.Add(EntityId, 2);
        return true;
    };

    for (const TPair<FGuid, FGuid>& Pair : ParentByEntity)
    {
        if (!VisitParent(Pair.Key))
        {
            return Fail(OutError, TEXT("Parent hierarchy contains a cycle."));
        }
    }
    return true;
}

bool FAkUGCSceneRuntime::ApplyCommand(
    const FAkUGCCommand& Command,
    const FAkUGCPrefabRegistry& Registry,
    TSet<FGuid>& OutAttachmentUpdates,
    FString* OutError)
{
    switch (Command.Type)
    {
    case EAkUGCCommandType::AddEntity:
        if (!SpawnEntity(Command.Entity, Registry, OutError))
        {
            return false;
        }
        OutAttachmentUpdates.Add(Command.Entity.EntityId);
        return true;

    case EAkUGCCommandType::DeleteEntity:
    {
        AActor* Actor = FindActor(Command.EntityId);
        if (!Actor)
        {
            if (ExternallyDeletedEntityIds.Remove(Command.EntityId) > 0)
            {
                OutAttachmentUpdates.Remove(Command.EntityId);
                return true;
            }
            return Fail(OutError, TEXT("Cannot delete a runtime entity that does not exist."));
        }

        TArray<AActor*> AttachedActors;
        Actor->GetAttachedActors(AttachedActors);
        for (AActor* AttachedActor : AttachedActors)
        {
            if (const UAkUGCEntityBindingComponent* ChildBinding = FindBinding(AttachedActor))
            {
                OutAttachmentUpdates.Add(ChildBinding->EntityId);
            }
        }

        if (!RemoveEntity(Command.EntityId))
        {
            return Fail(OutError, TEXT("Failed to delete runtime entity."));
        }
        OutAttachmentUpdates.Remove(Command.EntityId);
        return true;
    }

    case EAkUGCCommandType::SetTransform:
    case EAkUGCCommandType::SetProperty:
    case EAkUGCCommandType::RemoveProperty:
    case EAkUGCCommandType::SetParent:
    {
        AActor* Actor = FindActor(Command.EntityId);
        UAkUGCEntityBindingComponent* Binding = FindBinding(Actor);
        if (!Binding)
        {
            return Fail(OutError, TEXT("Runtime entity binding does not exist."));
        }

        FAkUGCEntityRecord UpdatedRecord = Binding->SourceRecord;
        if (Command.Type == EAkUGCCommandType::SetTransform)
        {
            UpdatedRecord.Transform = Command.Transform;
        }
        else if (Command.Type == EAkUGCCommandType::SetParent)
        {
            UpdatedRecord.ParentEntityId = Command.ParentEntityId;
            OutAttachmentUpdates.Add(Command.EntityId);
        }
        else
        {
            FAkUGCComponentRecord* Component = UpdatedRecord.Components.FindByPredicate([&Command](const FAkUGCComponentRecord& Candidate)
            {
                return Candidate.TypeId == Command.ComponentTypeId;
            });
            if (!Component)
            {
                return Fail(OutError, TEXT("Runtime entity component does not exist."));
            }

            if (Command.Type == EAkUGCCommandType::SetProperty)
            {
                Component->Properties.Add(Command.PropertyId, Command.PropertyValue);
            }
            else if (Component->Properties.Remove(Command.PropertyId) == 0)
            {
                return Fail(OutError, TEXT("Runtime entity property does not exist."));
            }
        }

        Binding->ApplyRecord(UpdatedRecord);
        return true;
    }

    case EAkUGCCommandType::DuplicateEntity:
    {
        AActor* SourceActor = FindActor(Command.SourceEntityId);
        const UAkUGCEntityBindingComponent* SourceBinding = FindBinding(SourceActor);
        if (!SourceBinding)
        {
            return Fail(OutError, TEXT("Runtime duplicate source entity does not exist."));
        }

        FAkUGCEntityRecord Duplicate = SourceBinding->SourceRecord;
        Duplicate.EntityId = Command.EntityId;
        Duplicate.Transform = Command.Transform;
        if (!SpawnEntity(Duplicate, Registry, OutError))
        {
            return false;
        }
        OutAttachmentUpdates.Add(Duplicate.EntityId);
        return true;
    }

    case EAkUGCCommandType::AddLogicNode:
    case EAkUGCCommandType::DeleteLogicNode:
    case EAkUGCCommandType::ConnectLogicNode:
    case EAkUGCCommandType::DisconnectLogicNode:
        return true;
    }

    return Fail(OutError, TEXT("Unsupported runtime command type."));
}

bool FAkUGCSceneRuntime::SpawnEntity(
    const FAkUGCEntityRecord& Entity,
    const FAkUGCPrefabRegistry& Registry,
    FString* OutError)
{
    UWorld* RuntimeWorld = World.Get();
    if (!RuntimeWorld)
    {
        return Fail(OutError, TEXT("Runtime world is not valid."));
    }
    if (Actors.Contains(Entity.EntityId))
    {
        return Fail(OutError, TEXT("Entity is already spawned."));
    }

    const FAkUGCPrefabDefinition* Definition = Registry.Find(Entity.PrefabId);
    if (!Definition)
    {
        return Fail(OutError, FString::Printf(TEXT("Prefab '%s' is not registered."), *Entity.PrefabId.ToString()));
    }

    UClass* ActorClass = AAkUGCRuntimeEntityActor::StaticClass();
    UStaticMesh* StaticMesh = nullptr;
    if (const FSoftObjectPath* AssetPath = Registry.ResolveAsset(Entity.PrefabId, GetCurrentPlatformVariant()))
    {
        if (AssetPath->IsValid())
        {
            UObject* Asset = AssetPath->TryLoad();
            if (!Asset)
            {
                return Fail(OutError, FString::Printf(TEXT("Failed to load prefab asset '%s'."), *AssetPath->ToString()));
            }
            if (UClass* LoadedClass = Cast<UClass>(Asset))
            {
                if (!LoadedClass->IsChildOf(AActor::StaticClass()))
                {
                    return Fail(OutError, TEXT("Prefab class asset must derive from AActor."));
                }
                ActorClass = LoadedClass;
            }
            else if (UStaticMesh* LoadedMesh = Cast<UStaticMesh>(Asset))
            {
                StaticMesh = LoadedMesh;
            }
            else
            {
                return Fail(OutError, TEXT("Prefab runtime asset must be an Actor class or StaticMesh."));
            }
        }
    }

    FActorSpawnParameters SpawnParameters;
    SpawnParameters.ObjectFlags |= RF_Transient;
    SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    SpawnParameters.Name = MakeUniqueObjectName(
        RuntimeWorld->PersistentLevel,
        ActorClass,
        FName(*FString::Printf(TEXT("UGC_%s"), *Entity.EntityId.ToString(EGuidFormats::Digits))));

    AActor* Actor = RuntimeWorld->SpawnActor<AActor>(ActorClass, Entity.Transform, SpawnParameters);
    if (!Actor)
    {
        return Fail(OutError, TEXT("Failed to spawn runtime actor."));
    }

    UAkUGCEntityBindingComponent* Binding = FindBinding(Actor);
    if (!Binding)
    {
        Binding = NewObject<UAkUGCEntityBindingComponent>(Actor, TEXT("UGCEntityBinding"), RF_Transient);
        Actor->AddInstanceComponent(Binding);
        Binding->RegisterComponent();
    }
    Binding->ApplyRecord(Entity);

    if (StaticMesh)
    {
        if (AAkUGCRuntimeEntityActor* RuntimeEntity = Cast<AAkUGCRuntimeEntityActor>(Actor))
        {
            RuntimeEntity->SetVisualMesh(StaticMesh);
        }
    }

    Actors.Add(Entity.EntityId, Actor);
    ExternallyDeletedEntityIds.Remove(Entity.EntityId);
    return true;
}

bool FAkUGCSceneRuntime::RefreshAttachments(const TSet<FGuid>& EntityIds, FString* OutError)
{
    TMap<FGuid, uint8> VisitStates;
    TArray<FGuid> OrderedEntityIds;
    TFunction<bool(const FGuid&)> Visit = [&](const FGuid& EntityId)
    {
        const uint8 State = VisitStates.FindRef(EntityId);
        if (State == 1)
        {
            return Fail(OutError, TEXT("Runtime parent hierarchy contains a cycle."));
        }
        if (State == 2)
        {
            return true;
        }

        AActor* Actor = FindActor(EntityId);
        const UAkUGCEntityBindingComponent* Binding = FindBinding(Actor);
        if (!Binding)
        {
            return Fail(OutError, TEXT("Expected runtime entity is missing while ordering attachments."));
        }

        VisitStates.Add(EntityId, 1);
        const FGuid ParentId = Binding->SourceRecord.ParentEntityId;
        if (ParentId.IsValid() && !Visit(ParentId))
        {
            return false;
        }
        VisitStates.Add(EntityId, 2);

        if (EntityIds.Contains(EntityId))
        {
            OrderedEntityIds.Add(EntityId);
        }
        return true;
    };

    TArray<FGuid> StableEntityIds = EntityIds.Array();
    StableEntityIds.Sort([](const FGuid& Left, const FGuid& Right)
    {
        return Left.ToString(EGuidFormats::Digits) < Right.ToString(EGuidFormats::Digits);
    });

    for (const FGuid& EntityId : StableEntityIds)
    {
        if (!Visit(EntityId))
        {
            return false;
        }
    }

    for (const FGuid& EntityId : OrderedEntityIds)
    {
        if (!RefreshAttachment(EntityId, OutError))
        {
            return false;
        }
    }
    return true;
}

bool FAkUGCSceneRuntime::RefreshAttachment(const FGuid& EntityId, FString* OutError)
{
    AActor* Actor = FindActor(EntityId);
    if (!Actor)
    {
        return Fail(OutError, TEXT("Expected runtime entity is missing while refreshing attachment."));
    }

    const UAkUGCEntityBindingComponent* Binding = FindBinding(Actor);
    if (!Binding)
    {
        return Fail(OutError, TEXT("Runtime entity binding does not exist while refreshing attachment."));
    }

    Actor->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
    if (!Binding->SourceRecord.ParentEntityId.IsValid())
    {
        return true;
    }

    AActor* Parent = FindActor(Binding->SourceRecord.ParentEntityId);
    if (!Parent)
    {
        return Fail(OutError, TEXT("Runtime parent entity does not exist."));
    }
    if (Parent == Actor)
    {
        return Fail(OutError, TEXT("Runtime entity cannot be parented to itself."));
    }

    if (!Actor->AttachToActor(Parent, FAttachmentTransformRules::KeepWorldTransform))
    {
        return Fail(OutError, TEXT("Unreal rejected the runtime entity attachment."));
    }
    return true;
}

bool FAkUGCSceneRuntime::AttachParents(const FAkUGCSceneDocument& Scene, FString* OutError)
{
    TSet<FGuid> EntityIds;
    for (const FAkUGCEntityRecord& Entity : Scene.Entities)
    {
        EntityIds.Add(Entity.EntityId);
    }
    return RefreshAttachments(EntityIds, OutError);
}

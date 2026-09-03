#include "Scene/AkUGCSceneRuntime.h"

#include "Command/AkUGCCommand.h"
#include "Components/StaticMeshComponent.h"
#include "Document/AkUGCDocument.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Entity/AkUGCEntityBindingComponent.h"
#include "Entity/AkUGCRuntimeEntityActor.h"
#include "Prefab/AkUGCPrefabRegistry.h"

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

    for (const TPair<FGuid, TWeakObjectPtr<AActor>>& Pair : Actors)
    {
        if (AActor* Actor = Pair.Value.Get())
        {
            Actor->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
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
        if (!ApplyCommand(Command, Registry, AttachmentUpdates, OutError))
        {
            return false;
        }
    }

    for (const FGuid& EntityId : AttachmentUpdates)
    {
        if (!RefreshAttachment(EntityId, OutError))
        {
            return false;
        }
    }
    return true;
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
    return true;
}

void FAkUGCSceneRuntime::Unload()
{
    for (TPair<FGuid, TWeakObjectPtr<AActor>>& Pair : Actors)
    {
        if (AActor* Actor = Pair.Value.Get())
        {
            Actor->Destroy();
        }
    }
    Actors.Reset();
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
        if (!RemoveEntity(Command.EntityId))
        {
            return Fail(OutError, TEXT("Cannot delete a runtime entity that does not exist."));
        }
        return true;

    case EAkUGCCommandType::SetTransform:
    case EAkUGCCommandType::SetProperty:
    case EAkUGCCommandType::RemoveProperty:
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
    return true;
}

bool FAkUGCSceneRuntime::RefreshAttachment(const FGuid& EntityId, FString* OutError)
{
    AActor* Actor = FindActor(EntityId);
    if (!Actor)
    {
        return true;
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

    Actor->AttachToActor(Parent, FAttachmentTransformRules::KeepWorldTransform);
    return true;
}

bool FAkUGCSceneRuntime::AttachParents(const FAkUGCSceneDocument& Scene, FString* OutError)
{
    for (const FAkUGCEntityRecord& Entity : Scene.Entities)
    {
        if (!Entity.ParentEntityId.IsValid())
        {
            continue;
        }

        AActor* Child = FindActor(Entity.EntityId);
        AActor* Parent = FindActor(Entity.ParentEntityId);
        if (!Child || !Parent)
        {
            return Fail(OutError, TEXT("Failed to resolve runtime parent attachment."));
        }
        if (Child == Parent)
        {
            return Fail(OutError, TEXT("Entity cannot be parented to itself."));
        }
        Child->AttachToActor(Parent, FAttachmentTransformRules::KeepWorldTransform);
    }
    return true;
}

#pragma once

#include "CoreMinimal.h"

struct FAkUGCCommand;
struct FAkUGCCommandTransaction;
struct FAkUGCEntityRecord;
struct FAkUGCPrefabDefinition;
struct FAkUGCSceneDocument;
class AActor;
class FAkUGCDocumentRuntimeSession;
class FAkUGCPrefabRegistry;
class UWorld;

class AKUGCASSETRUNTIME_API FAkUGCSceneRuntime
{
public:
    explicit FAkUGCSceneRuntime(UWorld* InWorld);
    ~FAkUGCSceneRuntime();

    bool LoadScene(
        const FAkUGCSceneDocument& Scene,
        const FAkUGCPrefabRegistry& Registry,
        FString* OutError = nullptr);

    bool SynchronizeScene(
        const FAkUGCSceneDocument& Scene,
        const FAkUGCPrefabRegistry& Registry,
        FString* OutError = nullptr);

    bool ApplyEntity(
        const FAkUGCEntityRecord& Entity,
        const FAkUGCPrefabRegistry& Registry,
        FString* OutError = nullptr);

    bool RemoveEntity(const FGuid& EntityId);
    bool NotifyActorDeletedExternally(const FGuid& EntityId, const AActor* Actor);
    void Unload();

    AActor* FindActor(const FGuid& EntityId) const;
    int32 Num() const;
    FGuid GetActiveSceneId() const;

    static FName GetCurrentPlatformVariant();

private:
    friend class FAkUGCDocumentRuntimeSession;

    bool ApplyTransaction(
        const FAkUGCCommandTransaction& Transaction,
        const FAkUGCPrefabRegistry& Registry,
        FString* OutError = nullptr);
    bool RunGameStartLogic(const FAkUGCSceneDocument& Scene, FString* OutError = nullptr);

    bool ValidateScene(
        const FAkUGCSceneDocument& Scene,
        const FAkUGCPrefabRegistry& Registry,
        FString* OutError) const;

    bool ApplyCommand(
        const FAkUGCCommand& Command,
        const FAkUGCPrefabRegistry& Registry,
        TSet<FGuid>& OutAttachmentUpdates,
        FString* OutError);

    bool SpawnEntity(
        const FAkUGCEntityRecord& Entity,
        const FAkUGCPrefabRegistry& Registry,
        FString* OutError);

    bool RefreshAttachments(const TSet<FGuid>& EntityIds, FString* OutError);
    bool RefreshAttachment(const FGuid& EntityId, FString* OutError);
    bool AttachParents(const FAkUGCSceneDocument& Scene, FString* OutError);

    TWeakObjectPtr<UWorld> World;
    FGuid ActiveSceneId;
    TMap<FGuid, TWeakObjectPtr<AActor>> Actors;
    TSet<FGuid> ExternallyDeletedEntityIds;
};

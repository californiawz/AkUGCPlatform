#pragma once

#include "CoreMinimal.h"

struct FAkUGCEntityRecord;
struct FAkUGCPrefabDefinition;
struct FAkUGCSceneDocument;
class AActor;
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
    void Unload();

    AActor* FindActor(const FGuid& EntityId) const;
    int32 Num() const;
    FGuid GetActiveSceneId() const;

    static FName GetCurrentPlatformVariant();

private:
    bool ValidateScene(
        const FAkUGCSceneDocument& Scene,
        const FAkUGCPrefabRegistry& Registry,
        FString* OutError) const;

    bool SpawnEntity(
        const FAkUGCEntityRecord& Entity,
        const FAkUGCPrefabRegistry& Registry,
        FString* OutError);

    bool AttachParents(const FAkUGCSceneDocument& Scene, FString* OutError);

    TWeakObjectPtr<UWorld> World;
    FGuid ActiveSceneId;
    TMap<FGuid, TWeakObjectPtr<AActor>> Actors;
};

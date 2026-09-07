#pragma once

#include "CoreMinimal.h"
#include "Gameplay/AkUGCTowerDefensePath.h"

struct FAkUGCCommand;
struct FAkUGCCommandTransaction;
struct FAkUGCEntityRecord;
struct FAkUGCLogicSpawnEffect;
struct FAkUGCLogicSpawnPlan;
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
    void CancelLogicExecution(const FGuid& ExecutionOwnerId);

    AActor* FindActor(const FGuid& EntityId) const;
    int32 Num() const;
    FGuid GetActiveSceneId() const;
    const FAkUGCTowerDefensePath& GetTowerDefensePath() const;

    static FName GetCurrentPlatformVariant();

private:
    friend class FAkUGCDocumentRuntimeSession;

    bool ApplyTransaction(
        const FAkUGCCommandTransaction& Transaction,
        const FAkUGCSceneDocument& ResultScene,
        const FAkUGCPrefabRegistry& Registry,
        FString* OutError = nullptr);
    bool RunGameStartLogic(
        const FAkUGCSceneDocument& Scene,
        const FAkUGCPrefabRegistry& Registry,
        const TWeakPtr<bool, ESPMode::ThreadSafe>& SessionLifetime,
        const FGuid& ExecutionOwnerId,
        bool bRequireAuthority,
        FString* OutError = nullptr);
    bool BuildLogicSpawnPlan(
        const FAkUGCLogicSpawnEffect& SpawnEffect,
        const FAkUGCPrefabRegistry& Registry,
        FAkUGCLogicSpawnPlan& OutPlan,
        FString& OutError) const;
    bool SpawnLogicPrefab(
        const FAkUGCLogicSpawnEffect& SpawnEffect,
        const FAkUGCPrefabRegistry& Registry,
        FGuid& OutEntityId,
        FString& OutError);

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
    TSharedRef<bool, ESPMode::ThreadSafe> LifetimeToken = MakeShared<bool, ESPMode::ThreadSafe>(true);
    FGuid ActiveSceneId;
    FGuid LogicExecutionOwnerId;
    FAkUGCTowerDefensePath TowerDefensePath;
    TMap<FGuid, TWeakObjectPtr<AActor>> Actors;
    TSet<FGuid> ExternallyDeletedEntityIds;
};

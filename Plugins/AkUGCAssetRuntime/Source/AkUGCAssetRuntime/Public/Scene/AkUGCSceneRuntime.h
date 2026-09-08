#pragma once

#include "CoreMinimal.h"
#include "Gameplay/AkUGCTowerDefenseMovement.h"
#include "Gameplay/AkUGCTowerDefensePath.h"

struct FAkUGCCommand;
struct FAkUGCCommandTransaction;
struct FAkUGCEntityRecord;
struct FAkUGCLogicSpawnEffect;
struct FAkUGCLogicSpawnPlan;
struct FAkUGCPrefabDefinition;
struct FAkUGCSceneDocument;
struct FAkUGCWaveRuntimeSnapshot;
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
    int32 GetActiveEnemyMovementCount() const;
    double GetBaseCurrentHealth() const;
    double GetBaseMaximumHealth() const;
    FGuid GetTowerDefenseBaseEntityId() const;
    FGuid GetTowerDefenseGoalEntityId() const;
    bool GetRuntimeHealth(const FGuid& EntityId, FAkUGCRuntimeHealth& OutHealth) const;
    bool ApplyRuntimeDamage(
        const FGuid& SourceEntityId,
        const FGuid& TargetEntityId,
        double Damage,
        FAkUGCRuntimeDamage& OutDamage,
        FString& OutError);
    bool GetEnemyRemainingPathDistance(const FGuid& EntityId, double& OutDistance) const;
    const TArray<FAkUGCTowerDefenseGoalReached>& GetGoalReachedEvents() const;
    bool AdvanceTowerDefenseMovement(
        double DeltaSeconds,
        double& OutAdvancedSeconds,
        bool& OutProcessedBoundary,
        FAkUGCTowerDefenseGameplayEvents& OutEvents,
        FString& OutError);
    bool HasActiveEnemyMovement() const;
    void ResetTowerDefenseMovement();

    /**
     * 沙箱受控生成入口：按 Prefab 生成一个运行时实体（复用逻辑 Spawn 语义）。
     * 成功返回 true 并回填新实体 ID；仅权威端可调用。
     */
    bool SpawnSandboxEntity(
        const FName& PrefabId,
        const FGuid& AnchorEntityId,
        const FAkUGCPrefabRegistry& Registry,
        FGuid& OutEntityId,
        FString& OutError);

    /** 沙箱受控规则集查询：读取当前波次/胜负快照（无规则集状态时返回 false）。 */
    bool GetWaveRuntimeState(FAkUGCWaveRuntimeSnapshot& OutSnapshot) const;

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
    bool RegisterEnemyMovement(
        const FAkUGCLogicSpawnEffect& SpawnEffect,
        const FAkUGCEntityRecord& Entity,
        FString& OutError);
    bool InitializeRuntimeHealth(const FAkUGCEntityRecord& Entity, FString& OutError);
    bool RegisterBasicTowerAttack(const FAkUGCEntityRecord& Entity, FString& OutError);
    bool SelectBasicTowerTarget(const FAkUGCTowerDefenseBasicTowerAttack& Tower, FGuid& OutTargetEntityId) const;
    bool BuildTowerDefenseRulesetRuntimeConfig(
        const FAkUGCSceneDocument& Scene,
        FAkUGCTowerDefenseRulesetRuntimeConfig& OutConfig,
        FString& OutError) const;
    bool ValidateTowerDefenseGameplay(
        const FAkUGCSceneDocument& Scene,
        const FAkUGCPrefabRegistry& Registry,
        FString* OutError = nullptr) const;
    bool InitializeTowerDefenseGameplay(
        const FAkUGCSceneDocument& Scene,
        const FAkUGCPrefabRegistry& Registry,
        FString* OutError = nullptr);
    void ResetTowerDefenseGameplay();

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
    FGuid TowerDefenseBaseEntityId;
    FGuid TowerDefenseGoalEntityId;
    TMap<FGuid, FAkUGCRuntimeHealth> RuntimeHealthByEntityId;
    TSet<FGuid> DeadEntityIds;
    TMap<FGuid, FAkUGCTowerDefenseBasicTowerAttack> BasicTowerAttacks;
    TMap<FGuid, FAkUGCTowerDefenseEnemyMovement> EnemyMovements;
    TSet<FGuid> RuntimeSpawnedEntityIds;
    TArray<FAkUGCTowerDefenseGoalReached> GoalReachedEvents;
    TMap<FGuid, TWeakObjectPtr<AActor>> Actors;
    TSet<FGuid> ExternallyDeletedEntityIds;
};

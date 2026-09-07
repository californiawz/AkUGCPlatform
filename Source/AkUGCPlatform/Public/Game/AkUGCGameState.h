#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameStateBase.h"
#include "Subsystem/AkUGCLogicRuntimeSubsystem.h"
#include "AkUGCGameState.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FAkUGCWaveSnapshotReplicatedDelegate);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FAkUGCBaseHealthReplicatedDelegate);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FAkUGCActiveEnemyCountReplicatedDelegate);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
    FAkUGCMatchEndedReplicatedDelegate,
    EAkUGCTowerDefenseMatchResult,
    Result);

/**
 * 塔防权威状态的网络复制载体。
 *
 * 服务器权威端通过 Project* 接口把 Scene Runtime 的可观察状态投影到
 * 这些复制属性；客户端只读，通过 OnRep 委托与 Getter 观察一致状态，
 * 不执行任何权威玩法。
 */
UCLASS()
class AKUGCPLATFORM_API AAkUGCGameState : public AGameStateBase
{
    GENERATED_BODY()

public:
    AAkUGCGameState();

    // ---- 服务器权威投影接口（仅权威端调用）----

    void ProjectWaveSnapshot(const FAkUGCWaveRuntimeSnapshot& Snapshot);
    void ProjectBaseHealth(double Current, double Maximum);
    void ProjectActiveEnemyCount(int32 Count);

    // ---- Blueprint 只读查询 ----

    UFUNCTION(BlueprintPure, Category = "UGC|Replication")
    FAkUGCWaveRuntimeSnapshot GetWaveSnapshot() const;

    UFUNCTION(BlueprintPure, Category = "UGC|Replication")
    EAkUGCTowerDefenseMatchResult GetMatchResult() const;

    UFUNCTION(BlueprintPure, Category = "UGC|Replication")
    double GetBaseHealthCurrent() const;

    UFUNCTION(BlueprintPure, Category = "UGC|Replication")
    double GetBaseHealthMaximum() const;

    UFUNCTION(BlueprintPure, Category = "UGC|Replication")
    int32 GetActiveEnemyCount() const;

    // ---- 复制回调事件（Blueprint 可监听）----

    UPROPERTY(BlueprintAssignable, Category = "UGC|Replication")
    FAkUGCWaveSnapshotReplicatedDelegate OnWaveSnapshotReplicated;

    UPROPERTY(BlueprintAssignable, Category = "UGC|Replication")
    FAkUGCBaseHealthReplicatedDelegate OnBaseHealthReplicated;

    UPROPERTY(BlueprintAssignable, Category = "UGC|Replication")
    FAkUGCActiveEnemyCountReplicatedDelegate OnActiveEnemyCountReplicated;

    UPROPERTY(BlueprintAssignable, Category = "UGC|Replication")
    FAkUGCMatchEndedReplicatedDelegate OnMatchEndedReplicated;

    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

    // ---- 复制回调（引擎在属性到达时调用；设为公开便于自动化测试直接验证）----

    UFUNCTION()
    void OnRep_WaveSnapshot();

    UFUNCTION()
    void OnRep_BaseHealth();

    UFUNCTION()
    void OnRep_ActiveEnemyCount();

protected:
    UPROPERTY(ReplicatedUsing = OnRep_WaveSnapshot)
    FAkUGCWaveRuntimeSnapshot ReplicatedWaveSnapshot;

    UPROPERTY(ReplicatedUsing = OnRep_BaseHealth)
    double ReplicatedBaseHealthCurrent = 0.0;

    UPROPERTY(ReplicatedUsing = OnRep_BaseHealth)
    double ReplicatedBaseHealthMaximum = 0.0;

    UPROPERTY(ReplicatedUsing = OnRep_ActiveEnemyCount)
    int32 ReplicatedActiveEnemyCount = 0;

    // 每个客户端本地维护，确保终局事件只广播一次。
    bool bMatchEndedBroadcast = false;
};

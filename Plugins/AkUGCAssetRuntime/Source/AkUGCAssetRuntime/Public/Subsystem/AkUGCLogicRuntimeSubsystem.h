#pragma once

#include "CoreMinimal.h"
#include "Document/AkUGCDocument.h"
#include "Gameplay/AkUGCTowerDefenseMovement.h"
#include "Logic/AkUGCLogicCompiler.h"
#include "Subsystems/WorldSubsystem.h"
#include "AkUGCLogicRuntimeSubsystem.generated.h"

struct FAkUGCLogicRunResult;
struct FAkUGCLogicSpawnEffect;

USTRUCT(BlueprintType)
struct AKUGCASSETRUNTIME_API FAkUGCLogicRuntimeMessage
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Logic")
    FGuid SourceNodeId;

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Logic")
    FString Message;
};

USTRUCT(BlueprintType)
struct AKUGCASSETRUNTIME_API FAkUGCLogicRuntimeSpawn
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Logic")
    FGuid SourceNodeId;

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Logic")
    FGuid EntityId;

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Logic")
    FName PrefabId;
};

USTRUCT(BlueprintType)
struct AKUGCASSETRUNTIME_API FAkUGCLogicRuntimeGoalReached
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Logic")
    FGuid SourceNodeId;

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Logic")
    FGuid EntityId;

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Logic")
    FGuid GoalEntityId;

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Logic")
    FGuid BaseEntityId;

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Logic")
    double DamageApplied = 0.0;

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Logic")
    double BaseHealthAfterDamage = 0.0;
};

USTRUCT(BlueprintType)
struct AKUGCASSETRUNTIME_API FAkUGCLogicRuntimeHealth
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Gameplay")
    FGuid EntityId;

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Gameplay")
    double Maximum = 0.0;

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Gameplay")
    double Current = 0.0;

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Gameplay")
    bool bIsDead = false;
};

USTRUCT(BlueprintType)
struct AKUGCASSETRUNTIME_API FAkUGCLogicRuntimeDamage
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Gameplay")
    FGuid SourceEntityId;

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Gameplay")
    FGuid TargetEntityId;

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Gameplay")
    double RequestedDamage = 0.0;

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Gameplay")
    double AppliedDamage = 0.0;

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Gameplay")
    double HealthAfterDamage = 0.0;

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Gameplay")
    bool bKilled = false;
};

USTRUCT(BlueprintType)
struct AKUGCASSETRUNTIME_API FAkUGCLogicRuntimeDeath
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Gameplay")
    FGuid SourceEntityId;

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Gameplay")
    FGuid EntityId;
};

USTRUCT(BlueprintType)
struct AKUGCASSETRUNTIME_API FAkUGCLogicRuntimeResult
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Logic")
    bool bSucceeded = false;

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Logic")
    FString ErrorPath;

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Logic")
    FString ErrorMessage;

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Logic")
    int32 ExecutedInstructionCount = 0;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
    FAkUGCLogicMessageDelegate,
    FGuid,
    SourceNodeId,
    const FString&,
    Message);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(
    FAkUGCLogicSpawnDelegate,
    FGuid,
    SourceNodeId,
    FGuid,
    EntityId,
    FName,
    PrefabId);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
    FAkUGCLogicGoalReachedDelegate,
    FGuid,
    SourceNodeId,
    FGuid,
    EntityId);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_FiveParams(
    FAkUGCRuntimeDamageDelegate,
    FGuid,
    SourceEntityId,
    FGuid,
    TargetEntityId,
    double,
    AppliedDamage,
    double,
    HealthAfterDamage,
    bool,
    bKilled);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
    FAkUGCRuntimeDeathDelegate,
    FGuid,
    SourceEntityId,
    FGuid,
    EntityId);

struct FAkUGCLogicSpawnPlan
{
    FGuid SourceNodeId;
    FName PrefabId;
    FGuid SpawnAtEntityId;
    int32 Count = 1;
    double IntervalSeconds = 0.0;
};

struct FAkUGCPendingLogicDelay
{
    double RemainingSeconds = 0.0;
    TArray<int32> SuccessorIndices;
};

struct FAkUGCPendingLogicSpawnBatch
{
    FAkUGCLogicSpawnPlan Plan;
    int32 RemainingCount = 0;
    double RemainingSeconds = 0.0;
};

UCLASS()
class AKUGCASSETRUNTIME_API UAkUGCLogicRuntimeSubsystem : public UTickableWorldSubsystem
{
    GENERATED_BODY()

public:
    UPROPERTY(BlueprintAssignable, Category = "UGC|Logic")
    FAkUGCLogicMessageDelegate OnMessage;

    UPROPERTY(BlueprintAssignable, Category = "UGC|Logic")
    FAkUGCLogicSpawnDelegate OnSpawn;

    UPROPERTY(BlueprintAssignable, Category = "UGC|Logic")
    FAkUGCLogicGoalReachedDelegate OnGoalReached;

    UPROPERTY(BlueprintAssignable, Category = "UGC|Gameplay")
    FAkUGCRuntimeDamageDelegate OnDamage;

    UPROPERTY(BlueprintAssignable, Category = "UGC|Gameplay")
    FAkUGCRuntimeDeathDelegate OnDeath;

    UFUNCTION(BlueprintCallable, Category = "UGC|Logic")
    FAkUGCLogicRuntimeResult RunGameStart(const FAkUGCLogicGraph& LogicGraph);

    FAkUGCLogicRuntimeResult RunGameStartForOwner(
        const FGuid& ExecutionOwnerId,
        const FAkUGCLogicGraph& LogicGraph);

    FAkUGCLogicRuntimeResult AdvanceLogicTime(double DeltaSeconds);

    UFUNCTION(BlueprintCallable, Category = "UGC|Logic")
    void ResetLogicRuntime();

    void ResetLogicRuntimeForOwner(const FGuid& ExecutionOwnerId);

    UFUNCTION(BlueprintPure, Category = "UGC|Logic")
    TArray<FAkUGCLogicRuntimeMessage> GetEmittedMessages() const;

    UFUNCTION(BlueprintPure, Category = "UGC|Logic")
    TArray<FAkUGCLogicRuntimeSpawn> GetSpawnedEntities() const;

    UFUNCTION(BlueprintPure, Category = "UGC|Logic")
    TArray<FAkUGCLogicRuntimeGoalReached> GetGoalReachedEntities() const;

    UFUNCTION(BlueprintPure, Category = "UGC|Gameplay")
    TArray<FAkUGCLogicRuntimeDamage> GetDamageEvents() const;

    UFUNCTION(BlueprintPure, Category = "UGC|Gameplay")
    TArray<FAkUGCLogicRuntimeDeath> GetDeathEvents() const;

    UFUNCTION(BlueprintPure, Category = "UGC|Gameplay")
    bool GetRuntimeHealth(FGuid EntityId, FAkUGCLogicRuntimeHealth& OutHealth) const;

    bool SetRuntimeHandlers(
        const FGuid& ExecutionOwnerId,
        TFunction<bool(const FAkUGCLogicSpawnEffect&, FAkUGCLogicSpawnPlan&, FString&)> InSpawnPlanHandler,
        TFunction<bool(const FAkUGCLogicSpawnEffect&, FGuid&, FString&)> InSpawnHandler,
        TFunction<bool(double, double&, bool&, FAkUGCTowerDefenseGameplayEvents&, FString&)> InAdvanceGameplayTimeHandler,
        TFunction<bool(const FGuid&, FAkUGCRuntimeHealth&)> InRuntimeHealthHandler,
        TFunction<bool()> InHasGameplayTimeWorkHandler,
        TFunction<void()> InResetGameplayHandler,
        TFunction<bool()> InRuntimeHandlerIsValid,
        FString* OutError = nullptr);

    virtual void Tick(float DeltaTime) override;
    virtual TStatId GetStatId() const override;
    virtual bool IsTickable() const override;

protected:
    virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
    bool ApplyRunResult(const FAkUGCLogicRunResult& RunResult, FString& OutErrorPath, FString& OutErrorMessage);
    bool AdvanceGameplayTime(double DeltaSeconds, FString& OutErrorPath, FString& OutErrorMessage);
    bool SpawnSingle(const FAkUGCLogicSpawnPlan& Plan, FString& OutErrorPath, FString& OutErrorMessage);
    FAkUGCLogicRuntimeResult MakeCurrentResult(bool bSucceeded, FString ErrorPath = {}, FString ErrorMessage = {}) const;
    void ClearExecutionState(bool bClearSpawnHandler);

    FAkUGCLogicProgram ActiveProgram;
    TArray<FAkUGCPendingLogicDelay> PendingDelays;
    TArray<FAkUGCPendingLogicSpawnBatch> PendingSpawnBatches;
    TArray<FAkUGCLogicRuntimeMessage> EmittedMessages;
    TArray<FAkUGCLogicRuntimeSpawn> SpawnedEntities;
    TArray<FAkUGCLogicRuntimeGoalReached> GoalReachedEntities;
    TArray<FAkUGCLogicRuntimeDamage> DamageEvents;
    TArray<FAkUGCLogicRuntimeDeath> DeathEvents;
    TFunction<bool(const FAkUGCLogicSpawnEffect&, FAkUGCLogicSpawnPlan&, FString&)> SpawnPlanHandler;
    TFunction<bool(const FAkUGCLogicSpawnEffect&, FGuid&, FString&)> SpawnHandler;
    TFunction<bool(double, double&, bool&, FAkUGCTowerDefenseGameplayEvents&, FString&)> AdvanceGameplayTimeHandler;
    TFunction<bool(const FGuid&, FAkUGCRuntimeHealth&)> RuntimeHealthHandler;
    TFunction<bool()> HasGameplayTimeWorkHandler;
    TFunction<void()> ResetGameplayHandler;
    TFunction<bool()> RuntimeHandlerIsValid;
    int32 TotalExecutedInstructionCount = 0;
    int32 ReservedSpawnCount = 0;
    FGuid ActiveExecutionOwnerId;
    FGuid ManualExecutionOwnerId = FGuid::NewGuid();
    bool bIsRunning = false;
    bool bResetRequested = false;
};

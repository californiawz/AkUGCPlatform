#pragma once

#include "CoreMinimal.h"
#include "Document/AkUGCDocument.h"
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

    bool SetSpawnHandlers(
        const FGuid& ExecutionOwnerId,
        TFunction<bool(const FAkUGCLogicSpawnEffect&, FAkUGCLogicSpawnPlan&, FString&)> InSpawnPlanHandler,
        TFunction<bool(const FAkUGCLogicSpawnEffect&, FGuid&, FString&)> InSpawnHandler,
        TFunction<bool()> InSpawnHandlerIsValid,
        FString* OutError = nullptr);

    virtual void Tick(float DeltaTime) override;
    virtual TStatId GetStatId() const override;
    virtual bool IsTickable() const override;

protected:
    virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
    bool ApplyRunResult(const FAkUGCLogicRunResult& RunResult, FString& OutErrorPath, FString& OutErrorMessage);
    bool SpawnSingle(const FAkUGCLogicSpawnPlan& Plan, FString& OutErrorPath, FString& OutErrorMessage);
    FAkUGCLogicRuntimeResult MakeCurrentResult(bool bSucceeded, FString ErrorPath = {}, FString ErrorMessage = {}) const;
    void ClearExecutionState(bool bClearSpawnHandler);

    FAkUGCLogicProgram ActiveProgram;
    TArray<FAkUGCPendingLogicDelay> PendingDelays;
    TArray<FAkUGCPendingLogicSpawnBatch> PendingSpawnBatches;
    TArray<FAkUGCLogicRuntimeMessage> EmittedMessages;
    TArray<FAkUGCLogicRuntimeSpawn> SpawnedEntities;
    TFunction<bool(const FAkUGCLogicSpawnEffect&, FAkUGCLogicSpawnPlan&, FString&)> SpawnPlanHandler;
    TFunction<bool(const FAkUGCLogicSpawnEffect&, FGuid&, FString&)> SpawnHandler;
    TFunction<bool()> SpawnHandlerIsValid;
    int32 TotalExecutedInstructionCount = 0;
    int32 ReservedSpawnCount = 0;
    FGuid ActiveExecutionOwnerId;
    FGuid ManualExecutionOwnerId = FGuid::NewGuid();
    bool bIsRunning = false;
    bool bResetRequested = false;
};

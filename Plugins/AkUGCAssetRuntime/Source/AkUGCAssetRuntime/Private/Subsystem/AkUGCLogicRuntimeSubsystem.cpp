#include "Subsystem/AkUGCLogicRuntimeSubsystem.h"

#include "Logic/AkUGCLogicCompiler.h"
#include "Logic/AkUGCLogicRunner.h"

FAkUGCLogicRuntimeResult UAkUGCLogicRuntimeSubsystem::RunGameStart(const FAkUGCLogicGraph& LogicGraph)
{
    if (bIsRunning)
    {
        return MakeCurrentResult(
            false,
            TEXT("logicRuntime.reentrantExecution"),
            TEXT("Logic runtime does not allow reentrant execution."));
    }
    TGuardValue<bool> RunningGuard(bIsRunning, true);
    ClearExecutionState(false);

    const FAkUGCLogicCompileResult CompileResult = FAkUGCLogicCompiler::Compile(LogicGraph);
    if (!CompileResult.bSucceeded)
    {
        return MakeCurrentResult(false, CompileResult.ErrorPath, CompileResult.ErrorMessage);
    }
    ActiveProgram = CompileResult.Program;

    const FAkUGCLogicRunResult RunResult = FAkUGCLogicRunner::RunGameStart(ActiveProgram);
    FString ErrorPath;
    FString ErrorMessage;
    if (!ApplyRunResult(RunResult, ErrorPath, ErrorMessage))
    {
        const FAkUGCLogicRuntimeResult FailureResult = MakeCurrentResult(
            false,
            MoveTemp(ErrorPath),
            MoveTemp(ErrorMessage));
        PendingDelays.Reset();
        PendingSpawnBatches.Reset();
        ReservedSpawnCount = SpawnedEntities.Num();
        if (bResetRequested)
        {
            ClearExecutionState(true);
            bResetRequested = false;
        }
        return FailureResult;
    }
    return MakeCurrentResult(true);
}

FAkUGCLogicRuntimeResult UAkUGCLogicRuntimeSubsystem::AdvanceLogicTime(double DeltaSeconds)
{
    if (!FMath::IsFinite(DeltaSeconds) || DeltaSeconds < 0.0)
    {
        return MakeCurrentResult(
            false,
            TEXT("logicRuntime.deltaSeconds"),
            TEXT("Logic time delta must be finite and non-negative."));
    }
    if (bIsRunning)
    {
        return MakeCurrentResult(
            false,
            TEXT("logicRuntime.reentrantExecution"),
            TEXT("Logic runtime does not allow reentrant execution."));
    }
    TGuardValue<bool> RunningGuard(bIsRunning, true);

    double RemainingDelta = DeltaSeconds;
    while (!PendingDelays.IsEmpty() || !PendingSpawnBatches.IsEmpty())
    {
        double NextEventSeconds = TNumericLimits<double>::Max();
        for (const FAkUGCPendingLogicDelay& Delay : PendingDelays)
        {
            NextEventSeconds = FMath::Min(NextEventSeconds, Delay.RemainingSeconds);
        }
        for (const FAkUGCPendingLogicSpawnBatch& Batch : PendingSpawnBatches)
        {
            NextEventSeconds = FMath::Min(NextEventSeconds, Batch.RemainingSeconds);
        }
        if (NextEventSeconds > RemainingDelta)
        {
            for (FAkUGCPendingLogicDelay& Delay : PendingDelays)
            {
                Delay.RemainingSeconds -= RemainingDelta;
            }
            for (FAkUGCPendingLogicSpawnBatch& Batch : PendingSpawnBatches)
            {
                Batch.RemainingSeconds -= RemainingDelta;
            }
            break;
        }

        for (FAkUGCPendingLogicDelay& Delay : PendingDelays)
        {
            Delay.RemainingSeconds -= NextEventSeconds;
        }
        for (FAkUGCPendingLogicSpawnBatch& Batch : PendingSpawnBatches)
        {
            Batch.RemainingSeconds -= NextEventSeconds;
        }
        RemainingDelta -= NextEventSeconds;

        TArray<FAkUGCPendingLogicDelay> DueDelays;
        for (int32 DelayIndex = PendingDelays.Num() - 1; DelayIndex >= 0; --DelayIndex)
        {
            if (PendingDelays[DelayIndex].RemainingSeconds <= 0.0)
            {
                DueDelays.Insert(MoveTemp(PendingDelays[DelayIndex]), 0);
                PendingDelays.RemoveAt(DelayIndex);
            }
        }
        for (const FAkUGCPendingLogicDelay& Delay : DueDelays)
        {
            const int32 RemainingBudget = AkUGCLogicLimits::MaxExecutedInstructions - TotalExecutedInstructionCount;
            const FAkUGCLogicRunResult RunResult = FAkUGCLogicRunner::RunFromInstructions(
                ActiveProgram,
                Delay.SuccessorIndices,
                RemainingBudget);
            FString ErrorPath;
            FString ErrorMessage;
            if (!ApplyRunResult(RunResult, ErrorPath, ErrorMessage))
            {
                const FAkUGCLogicRuntimeResult FailureResult = MakeCurrentResult(
                    false,
                    MoveTemp(ErrorPath),
                    MoveTemp(ErrorMessage));
                PendingDelays.Reset();
                PendingSpawnBatches.Reset();
                ReservedSpawnCount = SpawnedEntities.Num();
                if (bResetRequested)
                {
                    ClearExecutionState(true);
                    bResetRequested = false;
                }
                return FailureResult;
            }
        }

        for (int32 BatchIndex = 0; BatchIndex < PendingSpawnBatches.Num();)
        {
            FAkUGCPendingLogicSpawnBatch& Batch = PendingSpawnBatches[BatchIndex];
            if (Batch.RemainingSeconds > 0.0)
            {
                ++BatchIndex;
                continue;
            }

            FString ErrorPath;
            FString ErrorMessage;
            if (!SpawnSingle(Batch.Plan, ErrorPath, ErrorMessage))
            {
                const FAkUGCLogicRuntimeResult FailureResult = MakeCurrentResult(
                    false,
                    MoveTemp(ErrorPath),
                    MoveTemp(ErrorMessage));
                PendingDelays.Reset();
                PendingSpawnBatches.Reset();
                ReservedSpawnCount = SpawnedEntities.Num();
                if (bResetRequested)
                {
                    ClearExecutionState(true);
                    bResetRequested = false;
                }
                return FailureResult;
            }
            --Batch.RemainingCount;
            if (Batch.RemainingCount > 0)
            {
                Batch.RemainingSeconds += Batch.Plan.IntervalSeconds;
                ++BatchIndex;
            }
            else
            {
                PendingSpawnBatches.RemoveAt(BatchIndex);
            }
        }

        if (RemainingDelta <= 0.0)
        {
            break;
        }
    }
    return MakeCurrentResult(true);
}

void UAkUGCLogicRuntimeSubsystem::ResetLogicRuntime()
{
    if (bIsRunning)
    {
        bResetRequested = true;
        return;
    }
    ClearExecutionState(true);
}

TArray<FAkUGCLogicRuntimeMessage> UAkUGCLogicRuntimeSubsystem::GetEmittedMessages() const
{
    return EmittedMessages;
}

TArray<FAkUGCLogicRuntimeSpawn> UAkUGCLogicRuntimeSubsystem::GetSpawnedEntities() const
{
    return SpawnedEntities;
}

void UAkUGCLogicRuntimeSubsystem::SetSpawnHandlers(
    TFunction<bool(const FAkUGCLogicSpawnEffect&, FAkUGCLogicSpawnPlan&, FString&)> InSpawnPlanHandler,
    TFunction<bool(const FAkUGCLogicSpawnEffect&, FGuid&, FString&)> InSpawnHandler,
    TFunction<bool()> InSpawnHandlerIsValid)
{
    if (bIsRunning)
    {
        bResetRequested = true;
        return;
    }
    SpawnPlanHandler = MoveTemp(InSpawnPlanHandler);
    SpawnHandler = MoveTemp(InSpawnHandler);
    SpawnHandlerIsValid = MoveTemp(InSpawnHandlerIsValid);
}

void UAkUGCLogicRuntimeSubsystem::Tick(float DeltaTime)
{
    if (!PendingDelays.IsEmpty() || !PendingSpawnBatches.IsEmpty())
    {
        const FAkUGCLogicRuntimeResult Result = AdvanceLogicTime(static_cast<double>(DeltaTime));
        if (!Result.bSucceeded)
        {
            UE_LOG(LogTemp, Error, TEXT("UGC Logic runtime failed at %s: %s"), *Result.ErrorPath, *Result.ErrorMessage);
        }
    }
}

TStatId UAkUGCLogicRuntimeSubsystem::GetStatId() const
{
    RETURN_QUICK_DECLARE_CYCLE_STAT(UAkUGCLogicRuntimeSubsystem, STATGROUP_Tickables);
}

bool UAkUGCLogicRuntimeSubsystem::IsTickable() const
{
    return !PendingDelays.IsEmpty() || !PendingSpawnBatches.IsEmpty();
}

bool UAkUGCLogicRuntimeSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
    return WorldType == EWorldType::Game
        || WorldType == EWorldType::PIE
        || WorldType == EWorldType::GamePreview;
}

bool UAkUGCLogicRuntimeSubsystem::ApplyRunResult(
    const FAkUGCLogicRunResult& RunResult,
    FString& OutErrorPath,
    FString& OutErrorMessage)
{
    TotalExecutedInstructionCount += RunResult.ExecutedInstructionCount;
    if (!RunResult.bSucceeded)
    {
        OutErrorPath = RunResult.ErrorPath;
        OutErrorMessage = RunResult.ErrorMessage;
        return false;
    }

    for (const FAkUGCLogicMessageEvent& MessageEvent : RunResult.Messages)
    {
        FAkUGCLogicRuntimeMessage Message;
        Message.SourceNodeId = MessageEvent.SourceNodeId;
        Message.Message = MessageEvent.Message;
        EmittedMessages.Add(Message);
        UE_LOG(LogTemp, Display, TEXT("UGC Logic Message: %s"), *Message.Message);
        OnMessage.Broadcast(Message.SourceNodeId, Message.Message);
        if (bResetRequested)
        {
            OutErrorPath = TEXT("logicRuntime.cancelled");
            OutErrorMessage = TEXT("Logic execution was cancelled during a message callback.");
            return false;
        }
    }

    TArray<FAkUGCLogicSpawnPlan> SpawnPlans;
    SpawnPlans.Reserve(RunResult.SpawnEffects.Num());
    int32 NewReservedSpawnCount = ReservedSpawnCount;
    for (const FAkUGCLogicSpawnEffect& SpawnEffect : RunResult.SpawnEffects)
    {
        if (!SpawnPlanHandler || !SpawnHandler)
        {
            OutErrorPath = TEXT("logicRuntime.spawnHandler");
            OutErrorMessage = TEXT("Logic Spawn effect has no runtime handlers.");
            return false;
        }
        if (SpawnHandlerIsValid && !SpawnHandlerIsValid())
        {
            OutErrorPath = TEXT("logicRuntime.spawnHandler");
            OutErrorMessage = TEXT("Logic Spawn runtime owner is no longer valid.");
            SpawnPlanHandler = {};
            SpawnHandler = {};
            SpawnHandlerIsValid = {};
            return false;
        }

        FAkUGCLogicSpawnPlan Plan;
        FString PlanError;
        if (!SpawnPlanHandler(SpawnEffect, Plan, PlanError))
        {
            OutErrorPath = TEXT("logicRuntime.spawnPlan");
            OutErrorMessage = MoveTemp(PlanError);
            return false;
        }
        if (Plan.Count < 1
            || Plan.Count > AkUGCLogicLimits::MaxSpawnedEntitiesPerRun
            || Plan.PrefabId.IsNone()
            || (Plan.Count > 1 && (!FMath::IsFinite(Plan.IntervalSeconds) || Plan.IntervalSeconds <= 0.0)))
        {
            OutErrorPath = TEXT("logicRuntime.spawnPlan");
            OutErrorMessage = TEXT("Logic Spawn plan is invalid.");
            return false;
        }
        NewReservedSpawnCount += Plan.Count;
        if (NewReservedSpawnCount > AkUGCLogicLimits::MaxSpawnedEntitiesPerRun)
        {
            OutErrorPath = TEXT("logicRuntime.spawnBudget");
            OutErrorMessage = TEXT("Logic runtime Spawn budget exceeded.");
            return false;
        }
        SpawnPlans.Add(MoveTemp(Plan));
    }

    ReservedSpawnCount = NewReservedSpawnCount;
    for (const FAkUGCLogicSpawnPlan& Plan : SpawnPlans)
    {
        if (!SpawnSingle(Plan, OutErrorPath, OutErrorMessage))
        {
            return false;
        }
        if (Plan.Count > 1)
        {
            FAkUGCPendingLogicSpawnBatch& Batch = PendingSpawnBatches.AddDefaulted_GetRef();
            Batch.Plan = Plan;
            Batch.RemainingCount = Plan.Count - 1;
            Batch.RemainingSeconds = Plan.IntervalSeconds;
        }
    }

    for (const FAkUGCLogicDelayRequest& DelayRequest : RunResult.Delays)
    {
        FAkUGCPendingLogicDelay& Delay = PendingDelays.AddDefaulted_GetRef();
        Delay.RemainingSeconds = DelayRequest.DelaySeconds;
        Delay.SuccessorIndices = DelayRequest.SuccessorIndices;
    }
    return true;
}

bool UAkUGCLogicRuntimeSubsystem::SpawnSingle(
    const FAkUGCLogicSpawnPlan& Plan,
    FString& OutErrorPath,
    FString& OutErrorMessage)
{
    if (!SpawnHandler || (SpawnHandlerIsValid && !SpawnHandlerIsValid()))
    {
        OutErrorPath = TEXT("logicRuntime.spawnHandler");
        OutErrorMessage = TEXT("Logic Spawn runtime owner is no longer valid.");
        SpawnPlanHandler = {};
        SpawnHandler = {};
        SpawnHandlerIsValid = {};
        return false;
    }

    FAkUGCLogicSpawnEffect SpawnEffect;
    SpawnEffect.SourceNodeId = Plan.SourceNodeId;
    SpawnEffect.PrefabId = Plan.PrefabId;
    SpawnEffect.SpawnAtEntityId = Plan.SpawnAtEntityId;
    FGuid EntityId;
    FString SpawnError;
    if (!SpawnHandler(SpawnEffect, EntityId, SpawnError))
    {
        OutErrorPath = TEXT("logicRuntime.spawn");
        OutErrorMessage = MoveTemp(SpawnError);
        return false;
    }

    FAkUGCLogicRuntimeSpawn Spawn;
    Spawn.SourceNodeId = Plan.SourceNodeId;
    Spawn.EntityId = EntityId;
    Spawn.PrefabId = Plan.PrefabId;
    SpawnedEntities.Add(Spawn);
    OnSpawn.Broadcast(Spawn.SourceNodeId, Spawn.EntityId, Spawn.PrefabId);
    if (bResetRequested)
    {
        OutErrorPath = TEXT("logicRuntime.cancelled");
        OutErrorMessage = TEXT("Logic execution was cancelled during a Spawn callback.");
        return false;
    }
    return true;
}

FAkUGCLogicRuntimeResult UAkUGCLogicRuntimeSubsystem::MakeCurrentResult(
    bool bSucceeded,
    FString ErrorPath,
    FString ErrorMessage) const
{
    FAkUGCLogicRuntimeResult Result;
    Result.bSucceeded = bSucceeded;
    Result.ErrorPath = MoveTemp(ErrorPath);
    Result.ErrorMessage = MoveTemp(ErrorMessage);
    Result.ExecutedInstructionCount = TotalExecutedInstructionCount;
    return Result;
}

void UAkUGCLogicRuntimeSubsystem::ClearExecutionState(bool bClearSpawnHandler)
{
    ActiveProgram = FAkUGCLogicProgram{};
    PendingDelays.Reset();
    PendingSpawnBatches.Reset();
    EmittedMessages.Reset();
    SpawnedEntities.Reset();
    TotalExecutedInstructionCount = 0;
    ReservedSpawnCount = 0;
    bResetRequested = false;
    if (bClearSpawnHandler)
    {
        SpawnPlanHandler = {};
        SpawnHandler = {};
        SpawnHandlerIsValid = {};
    }
}

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
        PendingDelays.Reset();
        return MakeCurrentResult(false, MoveTemp(ErrorPath), MoveTemp(ErrorMessage));
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
    while (!PendingDelays.IsEmpty())
    {
        double NextDelay = PendingDelays[0].RemainingSeconds;
        for (const FAkUGCPendingLogicDelay& Delay : PendingDelays)
        {
            NextDelay = FMath::Min(NextDelay, Delay.RemainingSeconds);
        }
        if (NextDelay > RemainingDelta)
        {
            for (FAkUGCPendingLogicDelay& Delay : PendingDelays)
            {
                Delay.RemainingSeconds -= RemainingDelta;
            }
            break;
        }

        for (FAkUGCPendingLogicDelay& Delay : PendingDelays)
        {
            Delay.RemainingSeconds -= NextDelay;
        }
        RemainingDelta -= NextDelay;

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
                PendingDelays.Reset();
                return MakeCurrentResult(false, MoveTemp(ErrorPath), MoveTemp(ErrorMessage));
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
    if (!bIsRunning)
    {
        ClearExecutionState(true);
    }
}

TArray<FAkUGCLogicRuntimeMessage> UAkUGCLogicRuntimeSubsystem::GetEmittedMessages() const
{
    return EmittedMessages;
}

TArray<FAkUGCLogicRuntimeSpawn> UAkUGCLogicRuntimeSubsystem::GetSpawnedEntities() const
{
    return SpawnedEntities;
}

void UAkUGCLogicRuntimeSubsystem::SetSpawnHandler(
    TFunction<bool(const FAkUGCLogicSpawnEffect&, FGuid&, FString&)> InSpawnHandler,
    TFunction<bool()> InSpawnHandlerIsValid)
{
    SpawnHandler = MoveTemp(InSpawnHandler);
    SpawnHandlerIsValid = MoveTemp(InSpawnHandlerIsValid);
}

void UAkUGCLogicRuntimeSubsystem::Tick(float DeltaTime)
{
    if (!PendingDelays.IsEmpty())
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
    return !PendingDelays.IsEmpty();
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
    }

    for (const FAkUGCLogicSpawnEffect& SpawnEffect : RunResult.SpawnEffects)
    {
        if (!SpawnHandler)
        {
            OutErrorPath = TEXT("logicRuntime.spawnHandler");
            OutErrorMessage = TEXT("Logic Spawn effect has no runtime handler.");
            return false;
        }
        if (SpawnHandlerIsValid && !SpawnHandlerIsValid())
        {
            OutErrorPath = TEXT("logicRuntime.spawnHandler");
            OutErrorMessage = TEXT("Logic Spawn runtime owner is no longer valid.");
            SpawnHandler = {};
            SpawnHandlerIsValid = {};
            return false;
        }

        FGuid EntityId;
        FString SpawnError;
        if (!SpawnHandler(SpawnEffect, EntityId, SpawnError))
        {
            OutErrorPath = TEXT("logicRuntime.spawn");
            OutErrorMessage = MoveTemp(SpawnError);
            return false;
        }

        FAkUGCLogicRuntimeSpawn Spawn;
        Spawn.SourceNodeId = SpawnEffect.SourceNodeId;
        Spawn.EntityId = EntityId;
        Spawn.PrefabId = SpawnEffect.PrefabId;
        SpawnedEntities.Add(Spawn);
        OnSpawn.Broadcast(Spawn.SourceNodeId, Spawn.EntityId, Spawn.PrefabId);
    }

    for (const FAkUGCLogicDelayRequest& DelayRequest : RunResult.Delays)
    {
        FAkUGCPendingLogicDelay& Delay = PendingDelays.AddDefaulted_GetRef();
        Delay.RemainingSeconds = DelayRequest.DelaySeconds;
        Delay.SuccessorIndices = DelayRequest.SuccessorIndices;
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
    EmittedMessages.Reset();
    SpawnedEntities.Reset();
    TotalExecutedInstructionCount = 0;
    if (bClearSpawnHandler)
    {
        SpawnHandler = {};
        SpawnHandlerIsValid = {};
    }
}

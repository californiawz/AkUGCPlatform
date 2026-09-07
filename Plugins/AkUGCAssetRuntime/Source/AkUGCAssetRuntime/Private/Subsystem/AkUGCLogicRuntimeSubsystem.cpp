#include "Subsystem/AkUGCLogicRuntimeSubsystem.h"

#include "Logic/AkUGCLogicCompiler.h"
#include "Logic/AkUGCLogicRunner.h"

FAkUGCLogicRuntimeResult UAkUGCLogicRuntimeSubsystem::RunGameStart(const FAkUGCLogicGraph& LogicGraph)
{
    const UWorld* World = GetWorld();
    if (!World || World->GetNetMode() == NM_Client)
    {
        return MakeCurrentResult(
            false,
            TEXT("logicRuntime.authority"),
            TEXT("Manual Game Start execution requires a standalone or authoritative world."));
    }
    return RunGameStartForOwner(ManualExecutionOwnerId, LogicGraph);
}

FAkUGCLogicRuntimeResult UAkUGCLogicRuntimeSubsystem::RunGameStartForOwner(
    const FGuid& ExecutionOwnerId,
    const FAkUGCLogicGraph& LogicGraph)
{
    if (!ExecutionOwnerId.IsValid())
    {
        return MakeCurrentResult(
            false,
            TEXT("logicRuntime.executionOwnerId"),
            TEXT("Logic execution owner ID must be valid."));
    }
    if (ActiveExecutionOwnerId.IsValid() && ActiveExecutionOwnerId != ExecutionOwnerId)
    {
        return MakeCurrentResult(
            false,
            TEXT("logicRuntime.executionOwnerId"),
            TEXT("Logic runtime is already owned by another session."));
    }
    if (bIsRunning)
    {
        return MakeCurrentResult(
            false,
            TEXT("logicRuntime.reentrantExecution"),
            TEXT("Logic runtime does not allow reentrant execution."));
    }
    ActiveExecutionOwnerId = ExecutionOwnerId;
    TGuardValue<bool> RunningGuard(bIsRunning, true);
    ClearExecutionState(false);

    const FAkUGCLogicCompileResult CompileResult = FAkUGCLogicCompiler::Compile(LogicGraph);
    if (!CompileResult.bSucceeded)
    {
        return MakeCurrentResult(false, CompileResult.ErrorPath, CompileResult.ErrorMessage);
    }
    ActiveProgram = CompileResult.Program;
    if (ActiveProgram.WaveStartEntryIndex != INDEX_NONE
        && WaveConfig.Waves.Num() == AkUGCTowerDefenseRulesetLimits::RequiredWaveCount
        && ActiveEnemyCountHandler)
    {
        WaveState.CurrentWaveIndex = 0;
        WaveState.CurrentWaveId = WaveConfig.Waves[0].WaveId;
        WaveState.TotalWaveCount = WaveConfig.Waves.Num();
        if (!SetWaveState(EAkUGCWaveRuntimeState::WaitingToStart, WaveConfig.Waves[0].StartDelaySeconds))
        {
            const FAkUGCLogicRuntimeResult FailureResult = MakeCurrentResult(
                false,
                TEXT("logicRuntime.cancelled"),
                TEXT("Logic execution was cancelled during a wave state callback."));
            ClearExecutionState(true);
            return FailureResult;
        }
    }

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

FAkUGCLogicRuntimeResult UAkUGCLogicRuntimeSubsystem::RunWaveStart(int32 WaveIndex)
{
    return RunWaveStartForOwner(ManualExecutionOwnerId, WaveIndex);
}

FAkUGCLogicRuntimeResult UAkUGCLogicRuntimeSubsystem::RunWaveStartForOwner(
    const FGuid& ExecutionOwnerId,
    int32 WaveIndex)
{
    if (!ExecutionOwnerId.IsValid() || ActiveExecutionOwnerId != ExecutionOwnerId)
    {
        return MakeCurrentResult(
            false,
            TEXT("logicRuntime.executionOwnerId"),
            TEXT("Wave Start requires the active Logic execution owner."));
    }
    if (bIsRunning)
    {
        return MakeCurrentResult(
            false,
            TEXT("logicRuntime.reentrantExecution"),
            TEXT("Logic runtime does not allow reentrant execution."));
    }

    TGuardValue<bool> RunningGuard(bIsRunning, true);
    const int32 RemainingBudget = AkUGCLogicLimits::MaxExecutedInstructions - TotalExecutedInstructionCount;
    const FAkUGCLogicRunResult RunResult = FAkUGCLogicRunner::RunWaveStart(
        ActiveProgram,
        WaveIndex,
        RemainingBudget);
    FString ErrorPath;
    FString ErrorMessage;
    if (!ApplyRunResult(RunResult, ErrorPath, ErrorMessage))
    {
        const FAkUGCLogicRuntimeResult FailureResult = MakeCurrentResult(
            false,
            MoveTemp(ErrorPath),
            MoveTemp(ErrorMessage));
        if (bResetRequested)
        {
            ClearExecutionState(true);
            bResetRequested = false;
        }
        return FailureResult;
    }
    OnWaveStart.Broadcast(WaveIndex);
    if (bResetRequested || (RuntimeHandlerIsValid && !RuntimeHandlerIsValid()))
    {
        const FAkUGCLogicRuntimeResult FailureResult = MakeCurrentResult(
            false,
            TEXT("logicRuntime.cancelled"),
            TEXT("Logic execution was cancelled during a Wave Start callback."));
        ClearExecutionState(true);
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
    if (WaveState.Result != EAkUGCTowerDefenseMatchResult::InProgress)
    {
        return MakeCurrentResult(true);
    }

    double RemainingDelta = DeltaSeconds;
    while (true)
    {
        if (!RefreshWaveStateTransitions())
        {
            const FAkUGCLogicRuntimeResult FailureResult = MakeCurrentResult(
                false,
                TEXT("logicRuntime.waveState"),
                TEXT("Wave state validation failed or execution was cancelled during a callback."));
            ClearExecutionState(true);
            return FailureResult;
        }
        if (WaveState.Result != EAkUGCTowerDefenseMatchResult::InProgress)
        {
            return MakeCurrentResult(true);
        }
        const bool bHasWaveBoundary = WaveState.State == EAkUGCWaveRuntimeState::WaitingToStart
            || WaveState.State == EAkUGCWaveRuntimeState::BetweenWaves;
        const bool bHasLogicEvents = !PendingDelays.IsEmpty()
            || !PendingSpawnBatches.IsEmpty()
            || bHasWaveBoundary;
        if (RemainingDelta <= 0.0 && !bHasLogicEvents)
        {
            break;
        }
        if (!bHasLogicEvents)
        {
            FString ErrorPath;
            FString ErrorMessage;
            double GameplayAdvancedSeconds = 0.0;
            if (!AdvanceGameplayTime(RemainingDelta, GameplayAdvancedSeconds, ErrorPath, ErrorMessage))
            {
                const FAkUGCLogicRuntimeResult FailureResult = MakeCurrentResult(
                    false,
                    MoveTemp(ErrorPath),
                    MoveTemp(ErrorMessage));
                PendingDelays.Reset();
                PendingSpawnBatches.Reset();
                ClearExecutionState(true);
                return FailureResult;
            }
            RemainingDelta -= GameplayAdvancedSeconds;
            if (GameplayAdvancedSeconds == 0.0 && RemainingDelta > 0.0)
            {
                break;
            }
            continue;
        }

        double NextEventSeconds = TNumericLimits<double>::Max();
        for (const FAkUGCPendingLogicDelay& Delay : PendingDelays)
        {
            NextEventSeconds = FMath::Min(NextEventSeconds, Delay.RemainingSeconds);
        }
        for (const FAkUGCPendingLogicSpawnBatch& Batch : PendingSpawnBatches)
        {
            NextEventSeconds = FMath::Min(NextEventSeconds, Batch.RemainingSeconds);
        }
        if (bHasWaveBoundary)
        {
            NextEventSeconds = FMath::Min(NextEventSeconds, WaveState.SecondsUntilNextBoundary);
        }
        const double TimeSlice = FMath::Min(NextEventSeconds, RemainingDelta);
        FString GameplayErrorPath;
        FString GameplayErrorMessage;
        double GameplayAdvancedSeconds = 0.0;
        if (!AdvanceGameplayTime(
            TimeSlice,
            GameplayAdvancedSeconds,
            GameplayErrorPath,
            GameplayErrorMessage))
        {
            const FAkUGCLogicRuntimeResult FailureResult = MakeCurrentResult(
                false,
                MoveTemp(GameplayErrorPath),
                MoveTemp(GameplayErrorMessage));
            PendingDelays.Reset();
            PendingSpawnBatches.Reset();
            ClearExecutionState(true);
            return FailureResult;
        }
        for (FAkUGCPendingLogicDelay& Delay : PendingDelays)
        {
            Delay.RemainingSeconds -= GameplayAdvancedSeconds;
        }
        for (FAkUGCPendingLogicSpawnBatch& Batch : PendingSpawnBatches)
        {
            Batch.RemainingSeconds -= GameplayAdvancedSeconds;
        }
        if (bHasWaveBoundary)
        {
            WaveState.SecondsUntilNextBoundary = FMath::Max(
                0.0,
                WaveState.SecondsUntilNextBoundary - GameplayAdvancedSeconds);
        }
        RemainingDelta -= GameplayAdvancedSeconds;
        if (GameplayAdvancedSeconds < TimeSlice)
        {
            continue;
        }
        if (TimeSlice < NextEventSeconds)
        {
            break;
        }

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
                ClearExecutionState(true);
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
                ClearExecutionState(true);
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
                const bool bCompletedWaveBatch = Batch.bWaveBatch;
                PendingSpawnBatches.RemoveAt(BatchIndex);
                if (bCompletedWaveBatch
                    && !SetWaveState(EAkUGCWaveRuntimeState::WaitingForEnemies))
                {
                    const FAkUGCLogicRuntimeResult FailureResult = MakeCurrentResult(
                        false,
                        TEXT("logicRuntime.cancelled"),
                        TEXT("Logic execution was cancelled during a wave state callback."));
                    ClearExecutionState(true);
                    return FailureResult;
                }
            }
        }

        if (bHasWaveBoundary && WaveState.SecondsUntilNextBoundary <= 0.0)
        {
            FString WaveErrorPath;
            FString WaveErrorMessage;
            if (!StartCurrentWave(WaveErrorPath, WaveErrorMessage))
            {
                const FAkUGCLogicRuntimeResult FailureResult = MakeCurrentResult(
                    false,
                    MoveTemp(WaveErrorPath),
                    MoveTemp(WaveErrorMessage));
                ClearExecutionState(true);
                return FailureResult;
            }
        }
        if (!RefreshWaveStateTransitions())
        {
            const FAkUGCLogicRuntimeResult FailureResult = MakeCurrentResult(
                false,
                TEXT("logicRuntime.cancelled"),
                TEXT("Logic execution was cancelled during a wave state callback."));
            ClearExecutionState(true);
            return FailureResult;
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
    ResetLogicRuntimeForOwner(ManualExecutionOwnerId);
}

void UAkUGCLogicRuntimeSubsystem::ResetLogicRuntimeForOwner(const FGuid& ExecutionOwnerId)
{
    if (!ExecutionOwnerId.IsValid()
        || (ActiveExecutionOwnerId.IsValid() && ActiveExecutionOwnerId != ExecutionOwnerId))
    {
        return;
    }
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

TArray<FAkUGCLogicRuntimeGoalReached> UAkUGCLogicRuntimeSubsystem::GetGoalReachedEntities() const
{
    return GoalReachedEntities;
}

TArray<FAkUGCLogicRuntimeDamage> UAkUGCLogicRuntimeSubsystem::GetDamageEvents() const
{
    return DamageEvents;
}

TArray<FAkUGCLogicRuntimeDeath> UAkUGCLogicRuntimeSubsystem::GetDeathEvents() const
{
    return DeathEvents;
}

bool UAkUGCLogicRuntimeSubsystem::GetRuntimeHealth(
    FGuid EntityId,
    FAkUGCLogicRuntimeHealth& OutHealth) const
{
    OutHealth = FAkUGCLogicRuntimeHealth{};
    if (!RuntimeHealthHandler || (RuntimeHandlerIsValid && !RuntimeHandlerIsValid()))
    {
        return false;
    }

    FAkUGCRuntimeHealth Health;
    if (!RuntimeHealthHandler(EntityId, Health))
    {
        return false;
    }
    OutHealth.EntityId = EntityId;
    OutHealth.Maximum = Health.Maximum;
    OutHealth.Current = Health.Current;
    OutHealth.bIsDead = Health.Current <= 0.0;
    return true;
}

FAkUGCWaveRuntimeSnapshot UAkUGCLogicRuntimeSubsystem::GetWaveRuntimeState() const
{
    FAkUGCWaveRuntimeSnapshot Snapshot = WaveState;
    Snapshot.TotalWaveCount = WaveConfig.Waves.Num();
    return Snapshot;
}

bool UAkUGCLogicRuntimeSubsystem::ConfigureTowerDefenseWavesForOwner(
    const FGuid& ExecutionOwnerId,
    const FAkUGCTowerDefenseRulesetRuntimeConfig& Config,
    TFunction<int32()> InActiveEnemyCountHandler,
    FString* OutError)
{
    if (!ExecutionOwnerId.IsValid() || ActiveExecutionOwnerId != ExecutionOwnerId)
    {
        if (OutError)
        {
            *OutError = TEXT("Wave configuration requires the active Logic execution owner.");
        }
        return false;
    }
    if (bIsRunning)
    {
        if (OutError)
        {
            *OutError = TEXT("Wave configuration cannot change during Logic execution.");
        }
        return false;
    }
    if (Config.Waves.Num() != AkUGCTowerDefenseRulesetLimits::RequiredWaveCount
        || !InActiveEnemyCountHandler)
    {
        if (OutError)
        {
            *OutError = TEXT("Wave configuration requires exactly three waves and an active enemy handler.");
        }
        return false;
    }
    WaveConfig = Config;
    ActiveEnemyCountHandler = MoveTemp(InActiveEnemyCountHandler);
    WaveState = FAkUGCWaveRuntimeSnapshot{};
    WaveState.TotalWaveCount = WaveConfig.Waves.Num();
    return true;
}

bool UAkUGCLogicRuntimeSubsystem::SetRuntimeHandlers(
    const FGuid& ExecutionOwnerId,
    TFunction<bool(const FAkUGCLogicSpawnEffect&, FAkUGCLogicSpawnPlan&, FString&)> InSpawnPlanHandler,
    TFunction<bool(const FAkUGCLogicSpawnEffect&, FGuid&, FString&)> InSpawnHandler,
    TFunction<bool(double, double&, bool&, FAkUGCTowerDefenseGameplayEvents&, FString&)> InAdvanceGameplayTimeHandler,
    TFunction<bool(const FGuid&, FAkUGCRuntimeHealth&)> InRuntimeHealthHandler,
    TFunction<bool()> InHasGameplayTimeWorkHandler,
    TFunction<void()> InResetGameplayHandler,
    TFunction<bool()> InRuntimeHandlerIsValid,
    FString* OutError)
{
    if (!ExecutionOwnerId.IsValid())
    {
        if (OutError)
        {
            *OutError = TEXT("Logic execution owner ID must be valid.");
        }
        return false;
    }
    if (ActiveExecutionOwnerId.IsValid() && ActiveExecutionOwnerId != ExecutionOwnerId)
    {
        if (OutError)
        {
            *OutError = TEXT("Logic runtime is already owned by another session.");
        }
        return false;
    }
    if (bIsRunning)
    {
        if (OutError)
        {
            *OutError = TEXT("Logic runtime handlers cannot change during execution.");
        }
        return false;
    }
    ActiveExecutionOwnerId = ExecutionOwnerId;
    SpawnPlanHandler = MoveTemp(InSpawnPlanHandler);
    SpawnHandler = MoveTemp(InSpawnHandler);
    AdvanceGameplayTimeHandler = MoveTemp(InAdvanceGameplayTimeHandler);
    RuntimeHealthHandler = MoveTemp(InRuntimeHealthHandler);
    HasGameplayTimeWorkHandler = MoveTemp(InHasGameplayTimeWorkHandler);
    ResetGameplayHandler = MoveTemp(InResetGameplayHandler);
    RuntimeHandlerIsValid = MoveTemp(InRuntimeHandlerIsValid);
    return true;
}

void UAkUGCLogicRuntimeSubsystem::Tick(float DeltaTime)
{
    if (IsTickable())
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
    if (RuntimeHandlerIsValid && !RuntimeHandlerIsValid())
    {
        return false;
    }
    if (WaveState.Result != EAkUGCTowerDefenseMatchResult::InProgress)
    {
        return false;
    }
    return !PendingDelays.IsEmpty()
        || !PendingSpawnBatches.IsEmpty()
        || WaveState.State == EAkUGCWaveRuntimeState::WaitingToStart
        || WaveState.State == EAkUGCWaveRuntimeState::BetweenWaves
        || WaveState.State == EAkUGCWaveRuntimeState::WaitingForEnemies
        || (HasGameplayTimeWorkHandler && HasGameplayTimeWorkHandler());
}

bool UAkUGCLogicRuntimeSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
    return WorldType == EWorldType::Game
        || WorldType == EWorldType::PIE
        || WorldType == EWorldType::GamePreview;
}

bool UAkUGCLogicRuntimeSubsystem::SetWaveState(
    EAkUGCWaveRuntimeState NewState,
    double SecondsUntilBoundary)
{
    const bool bChanged = WaveState.State != NewState;
    WaveState.State = NewState;
    WaveState.SecondsUntilNextBoundary = FMath::Max(0.0, SecondsUntilBoundary);
    if (bChanged)
    {
        OnWaveStateChanged.Broadcast(WaveState.State, WaveState.CurrentWaveIndex);
    }
    return !bResetRequested && (!RuntimeHandlerIsValid || RuntimeHandlerIsValid());
}

bool UAkUGCLogicRuntimeSubsystem::RefreshWaveStateTransitions()
{
    if (WaveState.Result != EAkUGCTowerDefenseMatchResult::InProgress)
    {
        return true;
    }
    if (WaveConfig.DefeatCondition == EAkUGCTowerDefenseDefeatCondition::BaseHealthDepleted
        && WaveConfig.BaseEntityId.IsValid()
        && RuntimeHealthHandler)
    {
        FAkUGCRuntimeHealth BaseHealth;
        if (RuntimeHealthHandler(WaveConfig.BaseEntityId, BaseHealth) && BaseHealth.Current <= 0.0)
        {
            WaveState.Result = EAkUGCTowerDefenseMatchResult::Defeat;
            PendingDelays.Reset();
            PendingSpawnBatches.Reset();
            OnMatchEnded.Broadcast(WaveState.Result);
            return !bResetRequested && (!RuntimeHandlerIsValid || RuntimeHandlerIsValid());
        }
    }
    if (WaveState.State != EAkUGCWaveRuntimeState::WaitingForEnemies)
    {
        return true;
    }
    if (!ActiveEnemyCountHandler || (RuntimeHandlerIsValid && !RuntimeHandlerIsValid()))
    {
        bResetRequested = true;
        return false;
    }
    if (ActiveEnemyCountHandler() > 0)
    {
        return true;
    }

    const int32 NextWaveIndex = WaveState.CurrentWaveIndex + 1;
    if (!WaveConfig.Waves.IsValidIndex(NextWaveIndex))
    {
        if (!SetWaveState(EAkUGCWaveRuntimeState::Completed))
        {
            return false;
        }
        if (WaveConfig.VictoryCondition == EAkUGCTowerDefenseVictoryCondition::AllWavesCleared)
        {
            WaveState.Result = EAkUGCTowerDefenseMatchResult::Victory;
            OnMatchEnded.Broadcast(WaveState.Result);
            return !bResetRequested && (!RuntimeHandlerIsValid || RuntimeHandlerIsValid());
        }
        return true;
    }
    WaveState.CurrentWaveIndex = NextWaveIndex;
    WaveState.CurrentWaveId = WaveConfig.Waves[NextWaveIndex].WaveId;
    return SetWaveState(
        EAkUGCWaveRuntimeState::BetweenWaves,
        WaveConfig.WaveIntervalSeconds + WaveConfig.Waves[NextWaveIndex].StartDelaySeconds);
}

bool UAkUGCLogicRuntimeSubsystem::StartCurrentWave(
    FString& OutErrorPath,
    FString& OutErrorMessage)
{
    if (!WaveConfig.Waves.IsValidIndex(WaveState.CurrentWaveIndex)
        || ActiveProgram.WaveStartEntryIndex == INDEX_NONE)
    {
        OutErrorPath = TEXT("logicRuntime.waveState");
        OutErrorMessage = TEXT("Current wave state is not configured.");
        return false;
    }

    const int32 RemainingInstructionBudget = AkUGCLogicLimits::MaxExecutedInstructions - TotalExecutedInstructionCount;
    const FAkUGCLogicRunResult RunResult = FAkUGCLogicRunner::RunWaveStart(
        ActiveProgram,
        WaveState.CurrentWaveIndex,
        RemainingInstructionBudget);
    if (!ApplyRunResult(RunResult, OutErrorPath, OutErrorMessage))
    {
        return false;
    }

    if (!SetWaveState(EAkUGCWaveRuntimeState::Spawning))
    {
        OutErrorPath = TEXT("logicRuntime.cancelled");
        OutErrorMessage = TEXT("Logic execution was cancelled during a wave state callback.");
        return false;
    }
    OnWaveStart.Broadcast(WaveState.CurrentWaveIndex);
    if (bResetRequested)
    {
        OutErrorPath = TEXT("logicRuntime.cancelled");
        OutErrorMessage = TEXT("Logic execution was cancelled during a Wave Start callback.");
        return false;
    }

    const FAkUGCTowerDefenseWaveRuntimeConfig& Wave = WaveConfig.Waves[WaveState.CurrentWaveIndex];
    if (Wave.EnemyCount < 1
        || ReservedSpawnCount + Wave.EnemyCount > AkUGCLogicLimits::MaxSpawnedEntitiesPerRun)
    {
        OutErrorPath = TEXT("logicRuntime.waveSpawnBudget");
        OutErrorMessage = TEXT("Wave Spawn budget exceeded.");
        return false;
    }

    FAkUGCLogicSpawnPlan Plan;
    Plan.SourceNodeId = ActiveProgram.Instructions[ActiveProgram.WaveStartEntryIndex].SourceNodeId;
    Plan.PrefabId = Wave.EnemyPrefabId;
    Plan.SpawnAtEntityId = Wave.SpawnPointEntityId;
    Plan.Count = Wave.EnemyCount;
    Plan.IntervalSeconds = Wave.SpawnIntervalSeconds;
    ReservedSpawnCount += Plan.Count;
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
        Batch.bWaveBatch = true;
    }
    else if (!SetWaveState(EAkUGCWaveRuntimeState::WaitingForEnemies))
    {
        OutErrorPath = TEXT("logicRuntime.cancelled");
        OutErrorMessage = TEXT("Logic execution was cancelled during a wave state callback.");
        return false;
    }
    return true;
}

bool UAkUGCLogicRuntimeSubsystem::AdvanceGameplayTime(
    double DeltaSeconds,
    double& OutAdvancedSeconds,
    FString& OutErrorPath,
    FString& OutErrorMessage)
{
    OutAdvancedSeconds = 0.0;
    const EAkUGCWaveRuntimeState InitialWaveState = WaveState.State;
    if (!RefreshWaveStateTransitions())
    {
        OutErrorPath = TEXT("logicRuntime.cancelled");
        OutErrorMessage = TEXT("Logic execution was cancelled during a wave state callback.");
        return false;
    }
    if (WaveState.State != InitialWaveState || DeltaSeconds <= 0.0)
    {
        return true;
    }
    if (!HasGameplayTimeWorkHandler || !HasGameplayTimeWorkHandler())
    {
        OutAdvancedSeconds = DeltaSeconds;
        return true;
    }
    if (!AdvanceGameplayTimeHandler || (RuntimeHandlerIsValid && !RuntimeHandlerIsValid()))
    {
        OutErrorPath = TEXT("logicRuntime.gameplayTimeHandler");
        OutErrorMessage = TEXT("Gameplay time handler is no longer valid.");
        return false;
    }

    constexpr int32 MaxGameplaySlicesPerAdvance = 10000;
    double RemainingDelta = DeltaSeconds;
    bool bContinueAtCurrentTime = false;
    for (int32 SliceIndex = 0;
         (RemainingDelta > 0.0 || bContinueAtCurrentTime)
            && HasGameplayTimeWorkHandler
            && HasGameplayTimeWorkHandler();
         ++SliceIndex)
    {
        if (SliceIndex >= MaxGameplaySlicesPerAdvance)
        {
            OutErrorPath = TEXT("logicRuntime.gameplayTimeBudget");
            OutErrorMessage = TEXT("Gameplay time slice budget exceeded.");
            return false;
        }

        const EAkUGCWaveRuntimeState WaveStateBeforeSlice = WaveState.State;
        double AdvancedSeconds = 0.0;
        bool bProcessedBoundary = false;
        FAkUGCTowerDefenseGameplayEvents GameplayEvents;
        FString Error;
        if (!AdvanceGameplayTimeHandler(
            RemainingDelta,
            AdvancedSeconds,
            bProcessedBoundary,
            GameplayEvents,
            Error))
        {
            OutErrorPath = TEXT("logicRuntime.gameplayTime");
            OutErrorMessage = MoveTemp(Error);
            return false;
        }
        if (!FMath::IsFinite(AdvancedSeconds)
            || AdvancedSeconds < 0.0
            || AdvancedSeconds > RemainingDelta)
        {
            OutErrorPath = TEXT("logicRuntime.gameplayTime");
            OutErrorMessage = TEXT("Gameplay time handler returned an invalid consumed time.");
            return false;
        }
        RemainingDelta -= AdvancedSeconds;
        bContinueAtCurrentTime = bProcessedBoundary;
        if (AdvancedSeconds == 0.0 && !bProcessedBoundary)
        {
            break;
        }

        for (const FAkUGCRuntimeDamage& Damage : GameplayEvents.DamageEvents)
        {
            FAkUGCLogicRuntimeDamage Event;
            Event.SourceEntityId = Damage.SourceEntityId;
            Event.TargetEntityId = Damage.TargetEntityId;
            Event.RequestedDamage = Damage.RequestedDamage;
            Event.AppliedDamage = Damage.AppliedDamage;
            Event.HealthAfterDamage = Damage.HealthAfterDamage;
            Event.bKilled = Damage.bKilled;
            DamageEvents.Add(Event);
            OnDamage.Broadcast(
                Event.SourceEntityId,
                Event.TargetEntityId,
                Event.AppliedDamage,
                Event.HealthAfterDamage,
                Event.bKilled);
            if (bResetRequested)
            {
                OutErrorPath = TEXT("logicRuntime.cancelled");
                OutErrorMessage = TEXT("Logic execution was cancelled during a damage callback.");
                return false;
            }
            if (Event.bKilled)
            {
                FAkUGCLogicRuntimeDeath Death;
                Death.SourceEntityId = Event.SourceEntityId;
                Death.EntityId = Event.TargetEntityId;
                DeathEvents.Add(Death);
                OnDeath.Broadcast(Death.SourceEntityId, Death.EntityId);
            }
            if (bResetRequested)
            {
                OutErrorPath = TEXT("logicRuntime.cancelled");
                OutErrorMessage = TEXT("Logic execution was cancelled during a death callback.");
                return false;
            }
        }
        for (const FAkUGCTowerDefenseGoalReached& Reached : GameplayEvents.GoalReachedEvents)
        {
            FAkUGCLogicRuntimeGoalReached Event;
            Event.SourceNodeId = Reached.SourceNodeId;
            Event.EntityId = Reached.EntityId;
            Event.GoalEntityId = Reached.GoalEntityId;
            Event.BaseEntityId = Reached.BaseEntityId;
            Event.DamageApplied = Reached.DamageApplied;
            Event.BaseHealthAfterDamage = Reached.BaseHealthAfterDamage;
            GoalReachedEntities.Add(Event);
            OnGoalReached.Broadcast(Event.SourceNodeId, Event.EntityId);
            if (bResetRequested)
            {
                OutErrorPath = TEXT("logicRuntime.cancelled");
                OutErrorMessage = TEXT("Logic execution was cancelled during a GoalReached callback.");
                return false;
            }
        }
        if (!RefreshWaveStateTransitions())
        {
            OutErrorPath = TEXT("logicRuntime.cancelled");
            OutErrorMessage = TEXT("Logic execution was cancelled during a wave state callback.");
            return false;
        }
        if (WaveStateBeforeSlice == EAkUGCWaveRuntimeState::WaitingForEnemies
            && WaveState.State != WaveStateBeforeSlice)
        {
            break;
        }
    }
    OutAdvancedSeconds = DeltaSeconds - RemainingDelta;
    return true;
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
        if (RuntimeHandlerIsValid && !RuntimeHandlerIsValid())
        {
            OutErrorPath = TEXT("logicRuntime.spawnHandler");
            OutErrorMessage = TEXT("Logic Spawn runtime owner is no longer valid.");
            SpawnPlanHandler = {};
            SpawnHandler = {};
            RuntimeHandlerIsValid = {};
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
    if (!SpawnHandler || (RuntimeHandlerIsValid && !RuntimeHandlerIsValid()))
    {
        OutErrorPath = TEXT("logicRuntime.spawnHandler");
        OutErrorMessage = TEXT("Logic Spawn runtime owner is no longer valid.");
        SpawnPlanHandler = {};
        SpawnHandler = {};
        RuntimeHandlerIsValid = {};
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
    GoalReachedEntities.Reset();
    DamageEvents.Reset();
    DeathEvents.Reset();
    TotalExecutedInstructionCount = 0;
    ReservedSpawnCount = 0;
    WaveState = FAkUGCWaveRuntimeSnapshot{};
    WaveState.TotalWaveCount = WaveConfig.Waves.Num();
    bResetRequested = false;
    if (bClearSpawnHandler)
    {
        if (ResetGameplayHandler
            && (!RuntimeHandlerIsValid || RuntimeHandlerIsValid()))
        {
            ResetGameplayHandler();
        }
        SpawnPlanHandler = {};
        SpawnHandler = {};
        AdvanceGameplayTimeHandler = {};
        RuntimeHealthHandler = {};
        ActiveEnemyCountHandler = {};
        HasGameplayTimeWorkHandler = {};
        ResetGameplayHandler = {};
        WaveConfig = FAkUGCTowerDefenseRulesetRuntimeConfig{};
        WaveState = FAkUGCWaveRuntimeSnapshot{};
        RuntimeHandlerIsValid = {};
        ActiveExecutionOwnerId.Invalidate();
    }
}

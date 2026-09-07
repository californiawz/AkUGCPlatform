#include "Logic/AkUGCLogicRunner.h"

namespace
{
    FAkUGCLogicRunResult Failure(FString Path, FString Message)
    {
        FAkUGCLogicRunResult Result;
        Result.ErrorPath = MoveTemp(Path);
        Result.ErrorMessage = MoveTemp(Message);
        return Result;
    }

    bool ValidateProgram(const FAkUGCLogicProgram& Program, FString& OutPath, FString& OutMessage)
    {
        if (Program.Instructions.Num() > AkUGCLogicLimits::MaxNodes)
        {
            OutPath = TEXT("logicProgram.instructions");
            OutMessage = TEXT("Logic program exceeds the instruction budget.");
            return false;
        }

        int32 GameStartCount = 0;
        int32 WaveStartCount = 0;
        int32 TotalSuccessorCount = 0;
        TSet<FGuid> SourceNodeIds;
        for (int32 InstructionIndex = 0; InstructionIndex < Program.Instructions.Num(); ++InstructionIndex)
        {
            const FAkUGCLogicInstruction& Instruction = Program.Instructions[InstructionIndex];
            if (!Instruction.SourceNodeId.IsValid() || SourceNodeIds.Contains(Instruction.SourceNodeId))
            {
                OutPath = FString::Printf(TEXT("logicProgram.instructions[%d].sourceNodeId"), InstructionIndex);
                OutMessage = TEXT("Instruction source node ID must be valid and unique.");
                return false;
            }
            SourceNodeIds.Add(Instruction.SourceNodeId);
            switch (Instruction.Opcode)
            {
            case EAkUGCLogicOpcode::GameStart:
                ++GameStartCount;
                break;
            case EAkUGCLogicOpcode::WaveStart:
                ++WaveStartCount;
                break;
            case EAkUGCLogicOpcode::Message:
                if (Instruction.Operand.TrimStartAndEnd().IsEmpty()
                    || Instruction.Operand.Len() > AkUGCLogicLimits::MaxMessageLength)
                {
                    OutPath = FString::Printf(TEXT("logicProgram.instructions[%d].operand"), InstructionIndex);
                    OutMessage = TEXT("Message operand is invalid.");
                    return false;
                }
                break;
            case EAkUGCLogicOpcode::Timer:
                if (!FMath::IsFinite(Instruction.DelaySeconds)
                    || Instruction.DelaySeconds <= 0.0
                    || Instruction.DelaySeconds > AkUGCLogicLimits::MaxTimerDelaySeconds
                    || Instruction.SuccessorIndices.Num() != 1)
                {
                    OutPath = FString::Printf(TEXT("logicProgram.instructions[%d]"), InstructionIndex);
                    OutMessage = TEXT("Timer instruction is invalid.");
                    return false;
                }
                break;
            case EAkUGCLogicOpcode::Spawn:
                if (Instruction.SpawnPrefabId.IsNone() || !Instruction.SuccessorIndices.IsEmpty())
                {
                    OutPath = FString::Printf(TEXT("logicProgram.instructions[%d]"), InstructionIndex);
                    OutMessage = TEXT("Spawn instruction is invalid.");
                    return false;
                }
                break;
            default:
                OutPath = FString::Printf(TEXT("logicProgram.instructions[%d].opcode"), InstructionIndex);
                OutMessage = TEXT("Logic opcode is not supported.");
                return false;
            }

            TotalSuccessorCount += Instruction.SuccessorIndices.Num();
            if (TotalSuccessorCount > AkUGCLogicLimits::MaxConnections)
            {
                OutPath = TEXT("logicProgram.instructions.successorIndices");
                OutMessage = TEXT("Logic program exceeds the successor budget.");
                return false;
            }

            TSet<int32> UniqueSuccessors;
            for (int32 SuccessorIndex : Instruction.SuccessorIndices)
            {
                if (!Program.Instructions.IsValidIndex(SuccessorIndex))
                {
                    OutPath = FString::Printf(TEXT("logicProgram.instructions[%d].successorIndices"), InstructionIndex);
                    OutMessage = TEXT("Successor index is outside the instruction range.");
                    return false;
                }
                if (UniqueSuccessors.Contains(SuccessorIndex))
                {
                    OutPath = FString::Printf(TEXT("logicProgram.instructions[%d].successorIndices"), InstructionIndex);
                    OutMessage = TEXT("Successor index must be unique.");
                    return false;
                }
                UniqueSuccessors.Add(SuccessorIndex);
            }
        }

        if (Program.GameStartEntryIndex == INDEX_NONE)
        {
            if (GameStartCount != 0)
            {
                OutPath = TEXT("logicProgram.gameStartEntryIndex");
                OutMessage = TEXT("Game Start instruction requires an entry index.");
                return false;
            }
        }
        else if (!Program.Instructions.IsValidIndex(Program.GameStartEntryIndex)
            || GameStartCount != 1
            || Program.Instructions[Program.GameStartEntryIndex].Opcode != EAkUGCLogicOpcode::GameStart)
        {
            OutPath = TEXT("logicProgram.gameStartEntryIndex");
            OutMessage = TEXT("Game Start entry must reference the only Game Start instruction.");
            return false;
        }
        if (Program.WaveStartEntryIndex == INDEX_NONE)
        {
            if (WaveStartCount != 0)
            {
                OutPath = TEXT("logicProgram.waveStartEntryIndex");
                OutMessage = TEXT("Wave Start instruction requires an entry index.");
                return false;
            }
        }
        else if (!Program.Instructions.IsValidIndex(Program.WaveStartEntryIndex)
            || WaveStartCount != 1
            || Program.Instructions[Program.WaveStartEntryIndex].Opcode != EAkUGCLogicOpcode::WaveStart)
        {
            OutPath = TEXT("logicProgram.waveStartEntryIndex");
            OutMessage = TEXT("Wave Start entry must reference the only Wave Start instruction.");
            return false;
        }
        return true;
    }
}

FAkUGCLogicRunResult FAkUGCLogicRunner::RunGameStart(
    const FAkUGCLogicProgram& Program,
    int32 MaxExecutedInstructions)
{
    FString ErrorPath;
    FString ErrorMessage;
    if (!ValidateProgram(Program, ErrorPath, ErrorMessage))
    {
        return Failure(MoveTemp(ErrorPath), MoveTemp(ErrorMessage));
    }
    if (Program.GameStartEntryIndex == INDEX_NONE)
    {
        FAkUGCLogicRunResult Result;
        Result.bSucceeded = true;
        return Result;
    }
    return RunFromInstructions(Program, {Program.GameStartEntryIndex}, MaxExecutedInstructions);
}

FAkUGCLogicRunResult FAkUGCLogicRunner::RunWaveStart(
    const FAkUGCLogicProgram& Program,
    int32 WaveIndex,
    int32 MaxExecutedInstructions)
{
    if (WaveIndex < 0 || WaveIndex >= AkUGCTowerDefenseRulesetLimits::RequiredWaveCount)
    {
        return Failure(TEXT("logicProgram.waveIndex"), TEXT("Wave index is outside the supported range."));
    }
    FString ErrorPath;
    FString ErrorMessage;
    if (!ValidateProgram(Program, ErrorPath, ErrorMessage))
    {
        return Failure(MoveTemp(ErrorPath), MoveTemp(ErrorMessage));
    }
    if (Program.WaveStartEntryIndex == INDEX_NONE)
    {
        FAkUGCLogicRunResult Result;
        Result.bSucceeded = true;
        return Result;
    }
    return RunFromInstructions(Program, {Program.WaveStartEntryIndex}, MaxExecutedInstructions);
}

FAkUGCLogicRunResult FAkUGCLogicRunner::RunFromInstructions(
    const FAkUGCLogicProgram& Program,
    const TArray<int32>& EntryInstructionIndices,
    int32 MaxExecutedInstructions)
{
    if (MaxExecutedInstructions < 1
        || MaxExecutedInstructions > AkUGCLogicLimits::MaxExecutedInstructions)
    {
        return Failure(
            TEXT("logicProgram.maxExecutedInstructions"),
            TEXT("Execution budget must be positive and within the global limit."));
    }

    FString ErrorPath;
    FString ErrorMessage;
    if (!ValidateProgram(Program, ErrorPath, ErrorMessage))
    {
        return Failure(MoveTemp(ErrorPath), MoveTemp(ErrorMessage));
    }
    for (int32 EntryIndex : EntryInstructionIndices)
    {
        if (!Program.Instructions.IsValidIndex(EntryIndex))
        {
            return Failure(TEXT("logicProgram.entryInstructionIndices"), TEXT("Entry instruction index is outside the instruction range."));
        }
    }

    FAkUGCLogicRunResult Result;
    TArray<int32> PendingInstructionIndices = EntryInstructionIndices;
    int32 ReadIndex = 0;
    while (ReadIndex < PendingInstructionIndices.Num())
    {
        if (Result.ExecutedInstructionCount >= MaxExecutedInstructions)
        {
            Result.ErrorPath = TEXT("logicProgram.executionBudget");
            Result.ErrorMessage = TEXT("Logic execution exceeded its instruction budget.");
            return Result;
        }

        const FAkUGCLogicInstruction& Instruction = Program.Instructions[PendingInstructionIndices[ReadIndex++]];
        ++Result.ExecutedInstructionCount;
        switch (Instruction.Opcode)
        {
        case EAkUGCLogicOpcode::Message:
            Result.Messages.Add({Instruction.SourceNodeId, Instruction.Operand});
            break;
        case EAkUGCLogicOpcode::Timer:
            Result.Delays.Add({Instruction.SourceNodeId, Instruction.DelaySeconds, Instruction.SuccessorIndices});
            continue;
        case EAkUGCLogicOpcode::Spawn:
            Result.SpawnEffects.Add({Instruction.SourceNodeId, Instruction.SpawnPrefabId, Instruction.SpawnAtEntityId});
            break;
        case EAkUGCLogicOpcode::GameStart:
        case EAkUGCLogicOpcode::WaveStart:
            break;
        default:
            return Failure(TEXT("logicProgram.opcode"), TEXT("Logic opcode is not supported."));
        }
        PendingInstructionIndices.Append(Instruction.SuccessorIndices);
    }

    Result.bSucceeded = true;
    return Result;
}

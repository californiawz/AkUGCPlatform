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
}

FAkUGCLogicRunResult FAkUGCLogicRunner::RunGameStart(
    const FAkUGCLogicProgram& Program,
    int32 MaxExecutedInstructions)
{
    if (MaxExecutedInstructions < 1)
    {
        return Failure(TEXT("logicProgram.maxExecutedInstructions"), TEXT("Execution budget must be positive."));
    }
    if (Program.Instructions.Num() > AkUGCLogicLimits::MaxNodes)
    {
        return Failure(TEXT("logicProgram.instructions"), TEXT("Logic program exceeds the instruction budget."));
    }

    int32 GameStartCount = 0;
    for (int32 InstructionIndex = 0; InstructionIndex < Program.Instructions.Num(); ++InstructionIndex)
    {
        const FAkUGCLogicInstruction& Instruction = Program.Instructions[InstructionIndex];
        switch (Instruction.Opcode)
        {
        case EAkUGCLogicOpcode::GameStart:
            ++GameStartCount;
            break;
        case EAkUGCLogicOpcode::Message:
            break;
        default:
            return Failure(
                FString::Printf(TEXT("logicProgram.instructions[%d].opcode"), InstructionIndex),
                TEXT("Logic opcode is not supported."));
        }

        TSet<int32> UniqueSuccessors;
        for (int32 SuccessorIndex : Instruction.SuccessorIndices)
        {
            if (!Program.Instructions.IsValidIndex(SuccessorIndex))
            {
                return Failure(
                    FString::Printf(TEXT("logicProgram.instructions[%d].successorIndices"), InstructionIndex),
                    TEXT("Successor index is outside the instruction range."));
            }
            if (UniqueSuccessors.Contains(SuccessorIndex))
            {
                return Failure(
                    FString::Printf(TEXT("logicProgram.instructions[%d].successorIndices"), InstructionIndex),
                    TEXT("Successor index must be unique."));
            }
            UniqueSuccessors.Add(SuccessorIndex);
        }
    }

    if (Program.GameStartEntryIndex == INDEX_NONE)
    {
        if (GameStartCount != 0)
        {
            return Failure(TEXT("logicProgram.gameStartEntryIndex"), TEXT("Game Start instruction requires an entry index."));
        }
        FAkUGCLogicRunResult Result;
        Result.bSucceeded = true;
        return Result;
    }
    if (!Program.Instructions.IsValidIndex(Program.GameStartEntryIndex))
    {
        return Failure(TEXT("logicProgram.gameStartEntryIndex"), TEXT("Game Start entry index is outside the instruction range."));
    }
    if (GameStartCount != 1
        || Program.Instructions[Program.GameStartEntryIndex].Opcode != EAkUGCLogicOpcode::GameStart)
    {
        return Failure(TEXT("logicProgram.gameStartEntryIndex"), TEXT("Game Start entry must reference the only Game Start instruction."));
    }

    FAkUGCLogicRunResult Result;
    TArray<int32> PendingInstructionIndices = {Program.GameStartEntryIndex};
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
        if (Instruction.Opcode == EAkUGCLogicOpcode::Message)
        {
            Result.Messages.Add({Instruction.SourceNodeId, Instruction.Operand});
        }
        PendingInstructionIndices.Append(Instruction.SuccessorIndices);
    }

    Result.bSucceeded = true;
    return Result;
}

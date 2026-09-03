#include "Command/AkUGCCommandHistory.h"

FAkUGCCommandHistory::FAkUGCCommandHistory(int32 InMaxEntries)
    : MaxEntries(FMath::Max(1, InMaxEntries))
{
}

FAkUGCCommandExecutionResult FAkUGCCommandHistory::Execute(
    FAkUGCProjectDocument& Document,
    const FAkUGCCommandTransaction& Transaction)
{
    FAkUGCCommandTransaction Inverse;
    FAkUGCCommandExecutionResult Result = FAkUGCCommandExecutor::Apply(Document, Transaction, &Inverse);
    if (!Result.bSucceeded)
    {
        return Result;
    }

    UndoStack.Add({Transaction, MoveTemp(Inverse)});
    TrimUndoStack();
    RedoStack.Reset();
    return Result;
}

FAkUGCCommandExecutionResult FAkUGCCommandHistory::Undo(FAkUGCProjectDocument& Document)
{
    if (!CanUndo())
    {
        return FAkUGCCommandExecutionResult::Failure(TEXT("history.undo"), TEXT("There is no transaction to undo."));
    }

    const FEntry& Entry = UndoStack.Last();
    FAkUGCCommandExecutionResult Result = FAkUGCCommandExecutor::Apply(Document, Entry.Inverse);
    if (!Result.bSucceeded)
    {
        return Result;
    }

    RedoStack.Add(Entry);
    UndoStack.Pop(EAllowShrinking::No);
    return Result;
}

FAkUGCCommandExecutionResult FAkUGCCommandHistory::Redo(FAkUGCProjectDocument& Document)
{
    if (!CanRedo())
    {
        return FAkUGCCommandExecutionResult::Failure(TEXT("history.redo"), TEXT("There is no transaction to redo."));
    }

    const FEntry& Entry = RedoStack.Last();
    FAkUGCCommandExecutionResult Result = FAkUGCCommandExecutor::Apply(Document, Entry.Forward);
    if (!Result.bSucceeded)
    {
        return Result;
    }

    UndoStack.Add(Entry);
    TrimUndoStack();
    RedoStack.Pop(EAllowShrinking::No);
    return Result;
}

bool FAkUGCCommandHistory::CanUndo() const
{
    return !UndoStack.IsEmpty();
}

bool FAkUGCCommandHistory::CanRedo() const
{
    return !RedoStack.IsEmpty();
}

void FAkUGCCommandHistory::Reset()
{
    UndoStack.Reset();
    RedoStack.Reset();
}

void FAkUGCCommandHistory::TrimUndoStack()
{
    const int32 Excess = UndoStack.Num() - MaxEntries;
    if (Excess > 0)
    {
        UndoStack.RemoveAt(0, Excess, EAllowShrinking::No);
    }
}

#include "Command/AkUGCCommandHistory.h"

FAkUGCCommandHistory::FAkUGCCommandHistory(int32 InMaxEntries)
    : MaxEntries(FMath::Max(1, InMaxEntries))
{
}

FAkUGCCommandExecutionResult FAkUGCCommandHistory::Execute(
    FAkUGCProjectDocument& Document,
    const FAkUGCCommandTransaction& Transaction,
    FAkUGCCommandProjection Projection)
{
    TOptional<FAkUGCProjectDocument> BeforeDocument;
    if (Projection)
    {
        BeforeDocument.Emplace(Document);
    }

    FAkUGCCommandTransaction UndoTransaction;
    FAkUGCCommandExecutionResult Result = FAkUGCCommandExecutor::Apply(Document, Transaction, &UndoTransaction);
    if (!Result.bSucceeded)
    {
        return Result;
    }

    if (Projection)
    {
        Result = Projection(BeforeDocument.GetValue(), Document, Transaction);
        if (!Result.bSucceeded)
        {
            Document = MoveTemp(BeforeDocument.GetValue());
            return Result;
        }
    }

    UndoStack.Add(MoveTemp(UndoTransaction));
    TrimUndoStack();
    RedoStack.Reset();
    return Result;
}

FAkUGCCommandExecutionResult FAkUGCCommandHistory::Undo(
    FAkUGCProjectDocument& Document,
    FAkUGCCommandProjection Projection)
{
    if (!CanUndo())
    {
        return FAkUGCCommandExecutionResult::Failure(TEXT("history.undo"), TEXT("There is no transaction to undo."));
    }

    TOptional<FAkUGCProjectDocument> BeforeDocument;
    if (Projection)
    {
        BeforeDocument.Emplace(Document);
    }

    const FAkUGCCommandTransaction UndoTransaction = UndoStack.Last();
    FAkUGCCommandTransaction RedoTransaction;
    FAkUGCCommandExecutionResult Result = FAkUGCCommandExecutor::Apply(Document, UndoTransaction, &RedoTransaction);
    if (!Result.bSucceeded)
    {
        return Result;
    }

    if (Projection)
    {
        Result = Projection(BeforeDocument.GetValue(), Document, UndoTransaction);
        if (!Result.bSucceeded)
        {
            Document = MoveTemp(BeforeDocument.GetValue());
            return Result;
        }
    }

    UndoStack.Pop(EAllowShrinking::No);
    RedoStack.Add(MoveTemp(RedoTransaction));
    return Result;
}

FAkUGCCommandExecutionResult FAkUGCCommandHistory::Redo(
    FAkUGCProjectDocument& Document,
    FAkUGCCommandProjection Projection)
{
    if (!CanRedo())
    {
        return FAkUGCCommandExecutionResult::Failure(TEXT("history.redo"), TEXT("There is no transaction to redo."));
    }

    TOptional<FAkUGCProjectDocument> BeforeDocument;
    if (Projection)
    {
        BeforeDocument.Emplace(Document);
    }

    const FAkUGCCommandTransaction RedoTransaction = RedoStack.Last();
    FAkUGCCommandTransaction UndoTransaction;
    FAkUGCCommandExecutionResult Result = FAkUGCCommandExecutor::Apply(Document, RedoTransaction, &UndoTransaction);
    if (!Result.bSucceeded)
    {
        return Result;
    }

    if (Projection)
    {
        Result = Projection(BeforeDocument.GetValue(), Document, RedoTransaction);
        if (!Result.bSucceeded)
        {
            Document = MoveTemp(BeforeDocument.GetValue());
            return Result;
        }
    }

    RedoStack.Pop(EAllowShrinking::No);
    UndoStack.Add(MoveTemp(UndoTransaction));
    TrimUndoStack();
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

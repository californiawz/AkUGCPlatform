#pragma once

#include "CoreMinimal.h"
#include "Command/AkUGCCommandExecutor.h"

using FAkUGCCommandProjection = TFunction<FAkUGCCommandExecutionResult(
    const FAkUGCProjectDocument& Before,
    const FAkUGCProjectDocument& After,
    const FAkUGCCommandTransaction& AppliedTransaction)>;

class AKUGCCORE_API FAkUGCCommandHistory
{
public:
    explicit FAkUGCCommandHistory(int32 InMaxEntries = 100);

    FAkUGCCommandExecutionResult Execute(
        FAkUGCProjectDocument& Document,
        const FAkUGCCommandTransaction& Transaction,
        FAkUGCCommandProjection Projection = {});

    FAkUGCCommandExecutionResult Undo(
        FAkUGCProjectDocument& Document,
        FAkUGCCommandProjection Projection = {});

    FAkUGCCommandExecutionResult Redo(
        FAkUGCProjectDocument& Document,
        FAkUGCCommandProjection Projection = {});

    bool CanUndo() const;
    bool CanRedo() const;
    void Reset();

private:
    void TrimUndoStack();

    int32 MaxEntries;
    TArray<FAkUGCCommandTransaction> UndoStack;
    TArray<FAkUGCCommandTransaction> RedoStack;
};

#pragma once

#include "CoreMinimal.h"
#include "Command/AkUGCCommandExecutor.h"

class AKUGCCORE_API FAkUGCCommandHistory
{
public:
    explicit FAkUGCCommandHistory(int32 InMaxEntries = 100);

    FAkUGCCommandExecutionResult Execute(
        FAkUGCProjectDocument& Document,
        const FAkUGCCommandTransaction& Transaction);

    FAkUGCCommandExecutionResult Undo(FAkUGCProjectDocument& Document);
    FAkUGCCommandExecutionResult Redo(FAkUGCProjectDocument& Document);

    bool CanUndo() const;
    bool CanRedo() const;
    void Reset();

private:
    void TrimUndoStack();

    int32 MaxEntries;
    TArray<FAkUGCCommandTransaction> UndoStack;
    TArray<FAkUGCCommandTransaction> RedoStack;
};

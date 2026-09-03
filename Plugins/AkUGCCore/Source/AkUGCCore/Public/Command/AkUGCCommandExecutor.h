#pragma once

#include "CoreMinimal.h"
#include "Command/AkUGCCommand.h"

struct FAkUGCProjectDocument;

struct AKUGCCORE_API FAkUGCCommandExecutionResult
{
    bool bSucceeded = false;
    FString ErrorPath;
    FString ErrorMessage;

    static FAkUGCCommandExecutionResult Success();
    static FAkUGCCommandExecutionResult Failure(FString Path, FString Message);
};

class AKUGCCORE_API FAkUGCCommandExecutor
{
public:
    static FAkUGCCommandExecutionResult Apply(
        FAkUGCProjectDocument& Document,
        const FAkUGCCommandTransaction& Transaction,
        FAkUGCCommandTransaction* OutInverse = nullptr);

private:
    static FAkUGCCommandExecutionResult ApplySingle(
        FAkUGCProjectDocument& Document,
        const FAkUGCCommand& Command,
        FAkUGCCommand& OutInverse);
};

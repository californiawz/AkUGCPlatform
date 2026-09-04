#pragma once

#include "CoreMinimal.h"
#include "Logic/AkUGCLogicCompiler.h"

struct AKUGCCORE_API FAkUGCLogicMessageEvent
{
    FGuid SourceNodeId;
    FString Message;
};

struct AKUGCCORE_API FAkUGCLogicRunResult
{
    bool bSucceeded = false;
    FString ErrorPath;
    FString ErrorMessage;
    int32 ExecutedInstructionCount = 0;
    TArray<FAkUGCLogicMessageEvent> Messages;
};

class AKUGCCORE_API FAkUGCLogicRunner
{
public:
    static FAkUGCLogicRunResult RunGameStart(
        const FAkUGCLogicProgram& Program,
        int32 MaxExecutedInstructions = AkUGCLogicLimits::MaxExecutedInstructions);
};

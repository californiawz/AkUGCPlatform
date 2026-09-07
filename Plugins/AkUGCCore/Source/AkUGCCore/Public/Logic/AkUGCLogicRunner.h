#pragma once

#include "CoreMinimal.h"
#include "Logic/AkUGCLogicCompiler.h"

struct AKUGCCORE_API FAkUGCLogicMessageEvent
{
    FGuid SourceNodeId;
    FString Message;
};

struct AKUGCCORE_API FAkUGCLogicSpawnEffect
{
    FGuid SourceNodeId;
    FName PrefabId;
    FGuid SpawnAtEntityId;
};

struct AKUGCCORE_API FAkUGCLogicDelayRequest
{
    FGuid SourceNodeId;
    double DelaySeconds = 0.0;
    TArray<int32> SuccessorIndices;
};

struct AKUGCCORE_API FAkUGCLogicRunResult
{
    bool bSucceeded = false;
    FString ErrorPath;
    FString ErrorMessage;
    int32 ExecutedInstructionCount = 0;
    TArray<FAkUGCLogicMessageEvent> Messages;
    TArray<FAkUGCLogicSpawnEffect> SpawnEffects;
    TArray<FAkUGCLogicDelayRequest> Delays;
};

class AKUGCCORE_API FAkUGCLogicRunner
{
public:
    static FAkUGCLogicRunResult RunGameStart(
        const FAkUGCLogicProgram& Program,
        int32 MaxExecutedInstructions = AkUGCLogicLimits::MaxExecutedInstructions);
    static FAkUGCLogicRunResult RunWaveStart(
        const FAkUGCLogicProgram& Program,
        int32 WaveIndex,
        int32 MaxExecutedInstructions = AkUGCLogicLimits::MaxExecutedInstructions);

    static FAkUGCLogicRunResult RunFromInstructions(
        const FAkUGCLogicProgram& Program,
        const TArray<int32>& EntryInstructionIndices,
        int32 MaxExecutedInstructions = AkUGCLogicLimits::MaxExecutedInstructions);
};

#include "Subsystem/AkUGCLogicRuntimeSubsystem.h"

#include "Logic/AkUGCLogicCompiler.h"
#include "Logic/AkUGCLogicRunner.h"

FAkUGCLogicRuntimeResult UAkUGCLogicRuntimeSubsystem::RunGameStart(const FAkUGCLogicGraph& LogicGraph)
{
    FAkUGCLogicRuntimeResult RuntimeResult;
    if (bIsRunning)
    {
        RuntimeResult.ErrorPath = TEXT("logicRuntime.reentrantExecution");
        RuntimeResult.ErrorMessage = TEXT("Logic runtime does not allow reentrant execution.");
        return RuntimeResult;
    }
    TGuardValue<bool> RunningGuard(bIsRunning, true);
    EmittedMessages.Reset();

    const FAkUGCLogicCompileResult CompileResult = FAkUGCLogicCompiler::Compile(LogicGraph);
    if (!CompileResult.bSucceeded)
    {
        RuntimeResult.ErrorPath = CompileResult.ErrorPath;
        RuntimeResult.ErrorMessage = CompileResult.ErrorMessage;
        return RuntimeResult;
    }

    const FAkUGCLogicRunResult RunResult = FAkUGCLogicRunner::RunGameStart(CompileResult.Program);
    RuntimeResult.bSucceeded = RunResult.bSucceeded;
    RuntimeResult.ErrorPath = RunResult.ErrorPath;
    RuntimeResult.ErrorMessage = RunResult.ErrorMessage;
    RuntimeResult.ExecutedInstructionCount = RunResult.ExecutedInstructionCount;
    if (!RunResult.bSucceeded)
    {
        return RuntimeResult;
    }

    EmittedMessages.Reserve(RunResult.Messages.Num());
    for (const FAkUGCLogicMessageEvent& MessageEvent : RunResult.Messages)
    {
        FAkUGCLogicRuntimeMessage Message;
        Message.SourceNodeId = MessageEvent.SourceNodeId;
        Message.Message = MessageEvent.Message;
        EmittedMessages.Add(Message);
        UE_LOG(LogTemp, Display, TEXT("UGC Logic Message: %s"), *Message.Message);
        OnMessage.Broadcast(Message.SourceNodeId, Message.Message);
    }
    return RuntimeResult;
}

void UAkUGCLogicRuntimeSubsystem::ResetLogicRuntime()
{
    if (!bIsRunning)
    {
        EmittedMessages.Reset();
    }
}

TArray<FAkUGCLogicRuntimeMessage> UAkUGCLogicRuntimeSubsystem::GetEmittedMessages() const
{
    return EmittedMessages;
}

bool UAkUGCLogicRuntimeSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
    return WorldType == EWorldType::Game
        || WorldType == EWorldType::PIE
        || WorldType == EWorldType::GamePreview;
}

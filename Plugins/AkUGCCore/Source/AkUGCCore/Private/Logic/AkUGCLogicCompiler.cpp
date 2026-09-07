#include "Logic/AkUGCLogicCompiler.h"

#include "Validation/AkUGCDocumentValidator.h"

namespace
{
    bool GuidLess(const FGuid& Left, const FGuid& Right)
    {
        return Left.ToString(EGuidFormats::Digits) < Right.ToString(EGuidFormats::Digits);
    }
}

FAkUGCLogicCompileResult FAkUGCLogicCompiler::Compile(const FAkUGCLogicGraph& LogicGraph)
{
    FAkUGCLogicCompileResult Result;
    const FAkUGCValidationResult Validation = FAkUGCDocumentValidator::ValidateLogicGraph(LogicGraph);
    if (!Validation.IsValid())
    {
        const FAkUGCValidationIssue* FirstError = Validation.Issues.FindByPredicate([](const FAkUGCValidationIssue& Issue)
        {
            return Issue.Severity == EAkUGCValidationSeverity::Error;
        });
        Result.ErrorPath = FirstError ? FirstError->Path : TEXT("logicGraph");
        Result.ErrorMessage = FirstError ? FirstError->Message : TEXT("Logic graph validation failed.");
        return Result;
    }

    TMap<FGuid, const FAkUGCLogicNode*> NodesById;
    TMultiMap<FGuid, FGuid> TargetsBySource;
    TMap<FGuid, int32> IncomingCounts;
    for (const FAkUGCLogicNode& Node : LogicGraph.Nodes)
    {
        NodesById.Add(Node.NodeId, &Node);
        IncomingCounts.Add(Node.NodeId, 0);
    }
    for (const FAkUGCLogicConnection& Connection : LogicGraph.Connections)
    {
        TargetsBySource.Add(Connection.SourceNodeId, Connection.TargetNodeId);
        ++IncomingCounts.FindChecked(Connection.TargetNodeId);
    }

    TArray<FGuid> Ready;
    for (const TPair<FGuid, int32>& Pair : IncomingCounts)
    {
        if (Pair.Value == 0)
        {
            Ready.Add(Pair.Key);
        }
    }
    Ready.Sort(GuidLess);

    while (!Ready.IsEmpty())
    {
        const FGuid NodeId = Ready[0];
        Ready.RemoveAt(0);
        const FAkUGCLogicNode* Node = NodesById.FindChecked(NodeId);

        FAkUGCLogicInstruction& Instruction = Result.Program.Instructions.AddDefaulted_GetRef();
        switch (Node->Type)
        {
        case EAkUGCLogicNodeType::GameStart:
            Instruction.Opcode = EAkUGCLogicOpcode::GameStart;
            break;
        case EAkUGCLogicNodeType::Message:
            Instruction.Opcode = EAkUGCLogicOpcode::Message;
            break;
        case EAkUGCLogicNodeType::Timer:
            Instruction.Opcode = EAkUGCLogicOpcode::Timer;
            break;
        case EAkUGCLogicNodeType::Spawn:
            Instruction.Opcode = EAkUGCLogicOpcode::Spawn;
            break;
        default:
            Result.Program.Instructions.Reset();
            Result.ErrorPath = TEXT("logicGraph.nodes.type");
            Result.ErrorMessage = TEXT("Logic node type is not supported.");
            return Result;
        }
        Instruction.SourceNodeId = NodeId;
        Instruction.Operand = Node->Message;
        Instruction.DelaySeconds = Node->DelaySeconds;
        Instruction.SpawnPrefabId = Node->SpawnPrefabId;
        Instruction.SpawnAtEntityId = Node->SpawnAtEntityId;

        TArray<FGuid> Targets;
        TargetsBySource.MultiFind(NodeId, Targets);
        Targets.Sort(GuidLess);
        for (const FGuid& TargetId : Targets)
        {
            int32& IncomingCount = IncomingCounts.FindChecked(TargetId);
            --IncomingCount;
            if (IncomingCount == 0)
            {
                Ready.Add(TargetId);
                Ready.Sort(GuidLess);
            }
        }
    }

    if (Result.Program.Instructions.Num() != LogicGraph.Nodes.Num())
    {
        Result.Program.Instructions.Reset();
        Result.ErrorPath = TEXT("logicGraph.connections");
        Result.ErrorMessage = TEXT("Logic graph cannot be compiled because it contains a cycle.");
        return Result;
    }

    TMap<FGuid, int32> InstructionIndexByNodeId;
    for (int32 InstructionIndex = 0; InstructionIndex < Result.Program.Instructions.Num(); ++InstructionIndex)
    {
        const FAkUGCLogicInstruction& Instruction = Result.Program.Instructions[InstructionIndex];
        InstructionIndexByNodeId.Add(Instruction.SourceNodeId, InstructionIndex);
        if (Instruction.Opcode == EAkUGCLogicOpcode::GameStart)
        {
            Result.Program.GameStartEntryIndex = InstructionIndex;
        }
    }
    for (const FAkUGCLogicConnection& Connection : LogicGraph.Connections)
    {
        const int32 SourceIndex = InstructionIndexByNodeId.FindChecked(Connection.SourceNodeId);
        const int32 TargetIndex = InstructionIndexByNodeId.FindChecked(Connection.TargetNodeId);
        Result.Program.Instructions[SourceIndex].SuccessorIndices.Add(TargetIndex);
    }
    for (FAkUGCLogicInstruction& Instruction : Result.Program.Instructions)
    {
        Instruction.SuccessorIndices.Sort();
    }

    Result.bSucceeded = true;
    return Result;
}

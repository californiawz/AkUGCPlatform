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
        Instruction.Opcode = Node->Type == EAkUGCLogicNodeType::GameStart
            ? EAkUGCLogicOpcode::GameStart
            : EAkUGCLogicOpcode::Message;
        Instruction.SourceNodeId = NodeId;
        Instruction.Operand = Node->Message;

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

    Result.bSucceeded = true;
    return Result;
}

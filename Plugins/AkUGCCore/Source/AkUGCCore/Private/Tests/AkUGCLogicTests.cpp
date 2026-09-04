#include "Misc/AutomationTest.h"

#include "Algo/Reverse.h"
#include "Command/AkUGCCommandHistory.h"
#include "Command/AkUGCCommandJson.h"
#include "Document/AkUGCDocumentJson.h"
#include "Logic/AkUGCLogicCompiler.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
    FAkUGCProjectDocument MakeLogicDocument(FGuid& OutSceneId)
    {
        FAkUGCProjectDocument Document;
        Document.Manifest.ProjectId = FGuid::NewGuid();
        Document.Manifest.DisplayName = TEXT("Logic Test");
        Document.Manifest.TemplateId = TEXT("official.tower_defense");
        FAkUGCSceneDocument& Scene = Document.Scenes.AddDefaulted_GetRef();
        Scene.SceneId = FGuid::NewGuid();
        Scene.DisplayName = TEXT("Main");
        OutSceneId = Scene.SceneId;
        return Document;
    }

    FAkUGCCommand MakeLogicCommand(EAkUGCCommandType Type, const FGuid& SceneId)
    {
        FAkUGCCommand Command;
        Command.CommandId = FGuid::NewGuid();
        Command.Type = Type;
        Command.SceneId = SceneId;
        return Command;
    }

    FAkUGCCommandTransaction MakeLogicTransaction(TArray<FAkUGCCommand> Commands)
    {
        FAkUGCCommandTransaction Transaction;
        Transaction.TransactionId = FGuid::NewGuid();
        Transaction.Label = TEXT("Create start message graph");
        Transaction.Commands = MoveTemp(Commands);
        return Transaction;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCLogicGraphWorkflowTest,
    "AkUGC.Core.Logic.GameStartMessageWorkflow",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCLogicGraphWorkflowTest::RunTest(const FString& Parameters)
{
    FGuid SceneId;
    FAkUGCProjectDocument Document = MakeLogicDocument(SceneId);
    const FGuid StartId(1, 0, 0, 0);
    const FGuid MessageId(2, 0, 0, 0);

    FAkUGCCommand AddStart = MakeLogicCommand(EAkUGCCommandType::AddLogicNode, SceneId);
    AddStart.LogicNode.NodeId = StartId;
    AddStart.LogicNode.Type = EAkUGCLogicNodeType::GameStart;

    FAkUGCCommand AddMessage = MakeLogicCommand(EAkUGCCommandType::AddLogicNode, SceneId);
    AddMessage.LogicNode.NodeId = MessageId;
    AddMessage.LogicNode.Type = EAkUGCLogicNodeType::Message;
    AddMessage.LogicNode.Message = TEXT("Wave ready");

    FAkUGCCommand Connect = MakeLogicCommand(EAkUGCCommandType::ConnectLogicNode, SceneId);
    Connect.LogicConnection.SourceNodeId = StartId;
    Connect.LogicConnection.TargetNodeId = MessageId;

    const FAkUGCCommandTransaction Transaction = MakeLogicTransaction({AddStart, AddMessage, Connect});
    FAkUGCCommandHistory History;
    TestTrue(TEXT("Logic transaction succeeds"), History.Execute(Document, Transaction).bSucceeded);
    TestEqual(TEXT("Two logic nodes are stored"), Document.Scenes[0].LogicGraph.Nodes.Num(), 2);
    TestEqual(TEXT("Logic connection is stored"), Document.Scenes[0].LogicGraph.Connections.Num(), 1);

    const FAkUGCLogicCompileResult FirstCompile = FAkUGCLogicCompiler::Compile(Document.Scenes[0].LogicGraph);
    TestTrue(TEXT("Logic graph compiles"), FirstCompile.bSucceeded);
    TestEqual(TEXT("Two instructions are generated"), FirstCompile.Program.Instructions.Num(), 2);
    if (FirstCompile.Program.Instructions.Num() == 2)
    {
        TestEqual(TEXT("Game Start compiles first"), FirstCompile.Program.Instructions[0].Opcode, EAkUGCLogicOpcode::GameStart);
        TestEqual(TEXT("Message compiles second"), FirstCompile.Program.Instructions[1].Opcode, EAkUGCLogicOpcode::Message);
        TestEqual(TEXT("Message operand is preserved"), FirstCompile.Program.Instructions[1].Operand, FString(TEXT("Wave ready")));
    }

    FAkUGCLogicGraph ReorderedGraph = Document.Scenes[0].LogicGraph;
    Algo::Reverse(ReorderedGraph.Nodes);
    const FAkUGCLogicCompileResult ReorderedCompile = FAkUGCLogicCompiler::Compile(ReorderedGraph);
    TestTrue(TEXT("Reordered graph compiles"), ReorderedCompile.bSucceeded);
    if (FirstCompile.Program.Instructions.Num() == 2 && ReorderedCompile.Program.Instructions.Num() == 2)
    {
        TestEqual(TEXT("IR order does not depend on node storage order"),
            ReorderedCompile.Program.Instructions[0].SourceNodeId,
            FirstCompile.Program.Instructions[0].SourceNodeId);
        TestEqual(TEXT("IR operand remains deterministic"),
            ReorderedCompile.Program.Instructions[1].Operand,
            FirstCompile.Program.Instructions[1].Operand);
    }

    FString DocumentJson;
    FString Error;
    TestTrue(TEXT("Logic document serializes"), FAkUGCDocumentJson::Serialize(Document, DocumentJson, &Error));
    FAkUGCProjectDocument RestoredDocument;
    TestTrue(TEXT("Logic document deserializes"), FAkUGCDocumentJson::Deserialize(DocumentJson, RestoredDocument, &Error));
    TestEqual(TEXT("Logic nodes round-trip"), RestoredDocument.Scenes[0].LogicGraph.Nodes.Num(), 2);
    TestEqual(TEXT("Logic connections round-trip"), RestoredDocument.Scenes[0].LogicGraph.Connections.Num(), 1);

    FString CommandJson;
    TestTrue(TEXT("Logic transaction serializes"), FAkUGCCommandJson::Serialize(Transaction, CommandJson, &Error));
    FAkUGCCommandTransaction RestoredTransaction;
    TestTrue(TEXT("Logic transaction deserializes"), FAkUGCCommandJson::Deserialize(CommandJson, RestoredTransaction, &Error));
    TestEqual(TEXT("Logic command type round-trips"), RestoredTransaction.Commands[2].Type, EAkUGCCommandType::ConnectLogicNode);
    TestEqual(TEXT("Logic connection target round-trips"), RestoredTransaction.Commands[2].LogicConnection.TargetNodeId, MessageId);

    TestTrue(TEXT("Undo removes graph"), History.Undo(Document).bSucceeded);
    TestTrue(TEXT("Undo removes logic nodes"), Document.Scenes[0].LogicGraph.Nodes.IsEmpty());
    TestTrue(TEXT("Undo removes logic connections"), Document.Scenes[0].LogicGraph.Connections.IsEmpty());
    TestTrue(TEXT("Redo restores graph"), History.Redo(Document).bSucceeded);
    TestEqual(TEXT("Redo restores logic nodes"), Document.Scenes[0].LogicGraph.Nodes.Num(), 2);
    TestEqual(TEXT("Redo restores logic connection"), Document.Scenes[0].LogicGraph.Connections.Num(), 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCLogicGraphValidationTest,
    "AkUGC.Core.Logic.RejectsInvalidGraphs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCLogicGraphValidationTest::RunTest(const FString& Parameters)
{
    FGuid SceneId;
    FAkUGCProjectDocument Document = MakeLogicDocument(SceneId);
    FAkUGCLogicNode Start;
    Start.NodeId = FGuid(1, 0, 0, 0);
    Start.Type = EAkUGCLogicNodeType::GameStart;
    FAkUGCLogicNode Message;
    Message.NodeId = FGuid(2, 0, 0, 0);
    Message.Type = EAkUGCLogicNodeType::Message;
    Message.Message = TEXT("Hello");
    Document.Scenes[0].LogicGraph.Nodes = {Start, Message};

    FAkUGCLogicConnection InvalidDirection;
    InvalidDirection.SourceNodeId = Message.NodeId;
    InvalidDirection.TargetNodeId = Start.NodeId;
    Document.Scenes[0].LogicGraph.Connections = {InvalidDirection};
    TestFalse(TEXT("Message to Game Start connection is rejected"),
        FAkUGCLogicCompiler::Compile(Document.Scenes[0].LogicGraph).bSucceeded);

    Document.Scenes[0].LogicGraph.Connections[0].SourceNodeId = Start.NodeId;
    Document.Scenes[0].LogicGraph.Connections[0].TargetNodeId = FGuid::NewGuid();
    TestFalse(TEXT("Missing connection target is rejected"),
        FAkUGCLogicCompiler::Compile(Document.Scenes[0].LogicGraph).bSucceeded);

    Document.Scenes[0].LogicGraph.Connections.Reset();
    Document.Scenes[0].LogicGraph.Nodes[1].Message.Reset();
    TestFalse(TEXT("Empty message is rejected"),
        FAkUGCLogicCompiler::Compile(Document.Scenes[0].LogicGraph).bSucceeded);
    return true;
}

#endif

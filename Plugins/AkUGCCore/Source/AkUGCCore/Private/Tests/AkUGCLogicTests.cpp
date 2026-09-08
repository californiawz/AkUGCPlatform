#include "Misc/AutomationTest.h"

#include "Algo/Reverse.h"
#include "Command/AkUGCCommandHistory.h"
#include "Command/AkUGCCommandJson.h"
#include "Document/AkUGCDocumentJson.h"
#include "Logic/AkUGCLogicCompiler.h"
#include "Logic/AkUGCLogicRunner.h"

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
    FAkUGCLogicUpdateNodeTest,
    "AkUGC.Core.Logic.UpdateNodeParameter",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCLogicUpdateNodeTest::RunTest(const FString& Parameters)
{
    FGuid SceneId;
    FAkUGCProjectDocument Document = MakeLogicDocument(SceneId);
    const FGuid MessageId(2, 0, 0, 0);
    const FGuid SpawnId(3, 0, 0, 0);
    const FGuid AnchorA(4, 0, 0, 0);
    const FGuid AnchorB(5, 0, 0, 0);

    FAkUGCCommand AddAnchorA = MakeLogicCommand(EAkUGCCommandType::AddEntity, SceneId);
    AddAnchorA.Entity.EntityId = AnchorA;
    AddAnchorA.Entity.PrefabId = TEXT("official.gameplay.enemy_spawn");

    FAkUGCCommand AddAnchorB = MakeLogicCommand(EAkUGCCommandType::AddEntity, SceneId);
    AddAnchorB.Entity.EntityId = AnchorB;
    AddAnchorB.Entity.PrefabId = TEXT("official.gameplay.enemy_spawn");

    FAkUGCCommand AddMessage = MakeLogicCommand(EAkUGCCommandType::AddLogicNode, SceneId);
    AddMessage.LogicNode.NodeId = MessageId;
    AddMessage.LogicNode.Type = EAkUGCLogicNodeType::Message;
    AddMessage.LogicNode.Message = TEXT("Before");

    FAkUGCCommand AddSpawn = MakeLogicCommand(EAkUGCCommandType::AddLogicNode, SceneId);
    AddSpawn.LogicNode.NodeId = SpawnId;
    AddSpawn.LogicNode.Type = EAkUGCLogicNodeType::Spawn;
    AddSpawn.LogicNode.SpawnPrefabId = TEXT("official.unit.basic_enemy");
    AddSpawn.LogicNode.SpawnAtEntityId = AnchorA;

    FAkUGCCommandHistory History;
    TestTrue(TEXT("Add nodes succeeds"),
        History.Execute(Document, MakeLogicTransaction({AddAnchorA, AddAnchorB, AddMessage, AddSpawn})).bSucceeded);

    FAkUGCCommand UpdateMessage = MakeLogicCommand(EAkUGCCommandType::UpdateLogicNode, SceneId);
    UpdateMessage.LogicNode.NodeId = MessageId;
    UpdateMessage.LogicNode.Type = EAkUGCLogicNodeType::Message;
    UpdateMessage.LogicNode.Message = TEXT("After");

    FAkUGCCommand UpdateSpawn = MakeLogicCommand(EAkUGCCommandType::UpdateLogicNode, SceneId);
    UpdateSpawn.LogicNode.NodeId = SpawnId;
    UpdateSpawn.LogicNode.Type = EAkUGCLogicNodeType::Spawn;
    UpdateSpawn.LogicNode.SpawnPrefabId = TEXT("official.unit.basic_tower");
    UpdateSpawn.LogicNode.SpawnAtEntityId = AnchorB;

    const FAkUGCCommandTransaction UpdateTransaction = MakeLogicTransaction({UpdateMessage, UpdateSpawn});
    TestTrue(TEXT("Update nodes succeeds"), History.Execute(Document, UpdateTransaction).bSucceeded);

    const FAkUGCLogicNode* MessageNode = Document.Scenes[0].LogicGraph.Nodes.FindByPredicate(
        [&MessageId](const FAkUGCLogicNode& Node) { return Node.NodeId == MessageId; });
    TestTrue(TEXT("Updated message node exists"), MessageNode != nullptr);
    if (MessageNode)
    {
        TestEqual(TEXT("Message parameter is updated"), MessageNode->Message, FString(TEXT("After")));
    }

    const FAkUGCLogicNode* SpawnNode = Document.Scenes[0].LogicGraph.Nodes.FindByPredicate(
        [&SpawnId](const FAkUGCLogicNode& Node) { return Node.NodeId == SpawnId; });
    TestTrue(TEXT("Updated spawn node exists"), SpawnNode != nullptr);
    if (SpawnNode)
    {
        TestEqual(TEXT("Spawn prefab is updated"), SpawnNode->SpawnPrefabId, FName(TEXT("official.unit.basic_tower")));
        TestEqual(TEXT("Spawn anchor is updated"), SpawnNode->SpawnAtEntityId, AnchorB);
    }

    TestTrue(TEXT("Undo succeeds"), History.Undo(Document).bSucceeded);
    const FAkUGCLogicNode* UndoneMessage = Document.Scenes[0].LogicGraph.Nodes.FindByPredicate(
        [&MessageId](const FAkUGCLogicNode& Node) { return Node.NodeId == MessageId; });
    TestTrue(TEXT("Undone message node exists"), UndoneMessage != nullptr);
    if (UndoneMessage)
    {
        TestEqual(TEXT("Undo restores old message"), UndoneMessage->Message, FString(TEXT("Before")));
    }
    const FAkUGCLogicNode* UndoneSpawn = Document.Scenes[0].LogicGraph.Nodes.FindByPredicate(
        [&SpawnId](const FAkUGCLogicNode& Node) { return Node.NodeId == SpawnId; });
    TestTrue(TEXT("Undone spawn node exists"), UndoneSpawn != nullptr);
    if (UndoneSpawn)
    {
        TestEqual(TEXT("Undo restores old spawn prefab"), UndoneSpawn->SpawnPrefabId, FName(TEXT("official.unit.basic_enemy")));
        TestEqual(TEXT("Undo restores old spawn anchor"), UndoneSpawn->SpawnAtEntityId, AnchorA);
    }

    TestTrue(TEXT("Redo succeeds"), History.Redo(Document).bSucceeded);
    const FAkUGCLogicNode* RedoneMessage = Document.Scenes[0].LogicGraph.Nodes.FindByPredicate(
        [&MessageId](const FAkUGCLogicNode& Node) { return Node.NodeId == MessageId; });
    TestTrue(TEXT("Redone message node exists"), RedoneMessage != nullptr);
    if (RedoneMessage)
    {
        TestEqual(TEXT("Redo restores new message"), RedoneMessage->Message, FString(TEXT("After")));
    }

    FAkUGCCommand UpdateMissing = MakeLogicCommand(EAkUGCCommandType::UpdateLogicNode, SceneId);
    UpdateMissing.LogicNode.NodeId = FGuid(99, 0, 0, 0);
    UpdateMissing.LogicNode.Type = EAkUGCLogicNodeType::Message;
    UpdateMissing.LogicNode.Message = TEXT("Ghost");
    TestFalse(TEXT("Updating a missing node fails"),
        History.Execute(Document, MakeLogicTransaction({UpdateMissing})).bSucceeded);

    FString CommandJson;
    FString Error;
    TestTrue(TEXT("Update transaction serializes"), FAkUGCCommandJson::Serialize(UpdateTransaction, CommandJson, &Error));
    FAkUGCCommandTransaction RestoredTransaction;
    TestTrue(TEXT("Update transaction deserializes"), FAkUGCCommandJson::Deserialize(CommandJson, RestoredTransaction, &Error));
    TestEqual(TEXT("Update command count round-trips"), RestoredTransaction.Commands.Num(), 2);
    TestEqual(TEXT("Update command type round-trips"), RestoredTransaction.Commands[0].Type, EAkUGCCommandType::UpdateLogicNode);
    TestEqual(TEXT("Updated message round-trips"), RestoredTransaction.Commands[0].LogicNode.Message, FString(TEXT("After")));
    TestEqual(TEXT("Updated spawn anchor round-trips"), RestoredTransaction.Commands[1].LogicNode.SpawnAtEntityId, AnchorB);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCLogicRunnerTest,
    "AkUGC.Core.Logic.GameStartRunner",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCLogicRunnerTest::RunTest(const FString& Parameters)
{
    FAkUGCLogicNode IsolatedMessage;
    IsolatedMessage.NodeId = FGuid(1, 0, 0, 0);
    IsolatedMessage.Type = EAkUGCLogicNodeType::Message;
    IsolatedMessage.Message = TEXT("Do not emit");
    FAkUGCLogicNode Start;
    Start.NodeId = FGuid(2, 0, 0, 0);
    Start.Type = EAkUGCLogicNodeType::GameStart;
    FAkUGCLogicNode ConnectedMessage;
    ConnectedMessage.NodeId = FGuid(3, 0, 0, 0);
    ConnectedMessage.Type = EAkUGCLogicNodeType::Message;
    ConnectedMessage.Message = TEXT("Game started");
    FAkUGCLogicNode WaveStart;
    WaveStart.NodeId = FGuid(4, 0, 0, 0);
    WaveStart.Type = EAkUGCLogicNodeType::WaveStart;
    FAkUGCLogicNode WaveMessage;
    WaveMessage.NodeId = FGuid(5, 0, 0, 0);
    WaveMessage.Type = EAkUGCLogicNodeType::Message;
    WaveMessage.Message = TEXT("Wave started");

    FAkUGCLogicGraph Graph;
    Graph.Nodes = {WaveMessage, ConnectedMessage, WaveStart, Start, IsolatedMessage};
    FAkUGCLogicConnection Connection;
    Connection.SourceNodeId = Start.NodeId;
    Connection.TargetNodeId = ConnectedMessage.NodeId;
    Graph.Connections.Add(Connection);
    FAkUGCLogicConnection WaveConnection;
    WaveConnection.SourceNodeId = WaveStart.NodeId;
    WaveConnection.TargetNodeId = WaveMessage.NodeId;
    Graph.Connections.Add(WaveConnection);

    const FAkUGCLogicCompileResult CompileResult = FAkUGCLogicCompiler::Compile(Graph);
    TestTrue(TEXT("Runtime graph compiles"), CompileResult.bSucceeded);
    TestTrue(TEXT("Game Start entry is recorded"), CompileResult.Program.GameStartEntryIndex != INDEX_NONE);
    TestTrue(TEXT("Wave Start entry is recorded"), CompileResult.Program.WaveStartEntryIndex != INDEX_NONE);
    const FAkUGCLogicRunResult RunResult = FAkUGCLogicRunner::RunGameStart(CompileResult.Program);
    TestTrue(TEXT("Game Start execution succeeds"), RunResult.bSucceeded);
    TestEqual(TEXT("Only Game Start branch executes"), RunResult.ExecutedInstructionCount, 2);
    TestEqual(TEXT("Only Game Start message is emitted"), RunResult.Messages.Num(), 1);
    if (RunResult.Messages.Num() == 1)
    {
        TestEqual(TEXT("Connected message text is emitted"), RunResult.Messages[0].Message, FString(TEXT("Game started")));
        TestEqual(TEXT("Connected message source is preserved"), RunResult.Messages[0].SourceNodeId, ConnectedMessage.NodeId);
    }

    const FAkUGCLogicRunResult WaveRunResult = FAkUGCLogicRunner::RunWaveStart(CompileResult.Program, 1);
    TestTrue(TEXT("Wave Start execution succeeds"), WaveRunResult.bSucceeded);
    TestEqual(TEXT("Only Wave Start branch executes"), WaveRunResult.ExecutedInstructionCount, 2);
    TestEqual(TEXT("Wave Start emits its message"), WaveRunResult.Messages.Num(), 1);
    if (WaveRunResult.Messages.Num() == 1)
    {
        TestEqual(TEXT("Wave Start message is isolated"), WaveRunResult.Messages[0].Message, FString(TEXT("Wave started")));
    }
    TestFalse(TEXT("Negative Wave index is rejected"),
        FAkUGCLogicRunner::RunWaveStart(CompileResult.Program, -1).bSucceeded);
    TestFalse(TEXT("Wave index above Ruleset range is rejected"),
        FAkUGCLogicRunner::RunWaveStart(
            CompileResult.Program,
            AkUGCTowerDefenseRulesetLimits::RequiredWaveCount).bSucceeded);

    TestFalse(TEXT("Execution budget is enforced"),
        FAkUGCLogicRunner::RunGameStart(CompileResult.Program, 1).bSucceeded);
    FAkUGCLogicProgram MalformedProgram = CompileResult.Program;
    MalformedProgram.Instructions[MalformedProgram.GameStartEntryIndex].SuccessorIndices = {INDEX_NONE};
    TestFalse(TEXT("Invalid successor index is rejected"),
        FAkUGCLogicRunner::RunGameStart(MalformedProgram).bSucceeded);
    MalformedProgram = CompileResult.Program;
    MalformedProgram.Instructions[MalformedProgram.GameStartEntryIndex].SuccessorIndices = {1, 1};
    TestFalse(TEXT("Duplicate successor index is rejected"),
        FAkUGCLogicRunner::RunGameStart(MalformedProgram).bSucceeded);
    MalformedProgram = CompileResult.Program;
    MalformedProgram.GameStartEntryIndex = INDEX_NONE;
    MalformedProgram.Instructions[0].Opcode = static_cast<EAkUGCLogicOpcode>(255);
    TestFalse(TEXT("Malformed program without an entry is still rejected"),
        FAkUGCLogicRunner::RunGameStart(MalformedProgram).bSucceeded);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCLogicTimerSpawnTest,
    "AkUGC.Core.Logic.TimerSpawnScheduling",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCLogicTimerSpawnTest::RunTest(const FString& Parameters)
{
    FAkUGCLogicNode Start;
    Start.NodeId = FGuid(1, 0, 0, 0);
    Start.Type = EAkUGCLogicNodeType::GameStart;
    FAkUGCLogicNode Timer;
    Timer.NodeId = FGuid(2, 0, 0, 0);
    Timer.Type = EAkUGCLogicNodeType::Timer;
    Timer.DelaySeconds = 1.0;
    FAkUGCLogicNode Spawn;
    Spawn.NodeId = FGuid(3, 0, 0, 0);
    Spawn.Type = EAkUGCLogicNodeType::Spawn;
    Spawn.SpawnPrefabId = TEXT("official.unit.basic_enemy");
    Spawn.SpawnAtEntityId = FGuid(4, 0, 0, 0);

    FAkUGCLogicConnection StartToTimer;
    StartToTimer.SourceNodeId = Start.NodeId;
    StartToTimer.TargetNodeId = Timer.NodeId;
    FAkUGCLogicConnection TimerToSpawn;
    TimerToSpawn.SourceNodeId = Timer.NodeId;
    TimerToSpawn.TargetNodeId = Spawn.NodeId;
    FAkUGCLogicGraph Graph;
    Graph.Nodes = {Spawn, Start, Timer};
    Graph.Connections = {TimerToSpawn, StartToTimer};

    const FAkUGCLogicCompileResult CompileResult = FAkUGCLogicCompiler::Compile(Graph);
    TestTrue(TEXT("Timer Spawn graph compiles"), CompileResult.bSucceeded);
    const FAkUGCLogicRunResult StartResult = FAkUGCLogicRunner::RunGameStart(CompileResult.Program);
    TestTrue(TEXT("Game Start reaches Timer"), StartResult.bSucceeded);
    TestEqual(TEXT("Game Start and Timer execute immediately"), StartResult.ExecutedInstructionCount, 2);
    TestEqual(TEXT("Timer creates one delayed continuation"), StartResult.Delays.Num(), 1);
    TestTrue(TEXT("Spawn does not execute before delay"), StartResult.SpawnEffects.IsEmpty());
    if (StartResult.Delays.Num() != 1)
    {
        return false;
    }
    TestEqual(TEXT("Timer delay compiles exactly"), StartResult.Delays[0].DelaySeconds, 1.0);

    const FAkUGCLogicRunResult ContinueResult = FAkUGCLogicRunner::RunFromInstructions(
        CompileResult.Program,
        StartResult.Delays[0].SuccessorIndices);
    TestTrue(TEXT("Delayed continuation succeeds"), ContinueResult.bSucceeded);
    TestEqual(TEXT("Delayed continuation executes Spawn"), ContinueResult.ExecutedInstructionCount, 1);
    TestEqual(TEXT("Spawn emits one effect"), ContinueResult.SpawnEffects.Num(), 1);
    if (ContinueResult.SpawnEffects.Num() == 1)
    {
        TestEqual(TEXT("Spawn Prefab is preserved"), ContinueResult.SpawnEffects[0].PrefabId, FName(TEXT("official.unit.basic_enemy")));
        TestEqual(TEXT("Spawn anchor is preserved"), ContinueResult.SpawnEffects[0].SpawnAtEntityId, Spawn.SpawnAtEntityId);
    }
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

    Document.Scenes[0].LogicGraph.Nodes[1].Message = TEXT("Valid");
    Document.Scenes[0].LogicGraph.Nodes[1].Type = static_cast<EAkUGCLogicNodeType>(255);
    TestFalse(TEXT("Unknown node type is rejected"),
        FAkUGCLogicCompiler::Compile(Document.Scenes[0].LogicGraph).bSucceeded);

    FAkUGCLogicNode WaveStart;
    WaveStart.NodeId = FGuid::NewGuid();
    WaveStart.Type = EAkUGCLogicNodeType::WaveStart;
    FAkUGCLogicNode Spawn;
    Spawn.NodeId = FGuid::NewGuid();
    Spawn.Type = EAkUGCLogicNodeType::Spawn;
    Spawn.SpawnPrefabId = TEXT("official.unit.basic_enemy");
    FAkUGCLogicGraph WaveSpawnGraph;
    WaveSpawnGraph.Nodes = {WaveStart, Spawn};
    TestFalse(TEXT("Wave Start graphs reject duplicate Spawn ownership"),
        FAkUGCLogicCompiler::Compile(WaveSpawnGraph).bSucceeded);

    FAkUGCLogicGraph RepeatedWaveBudgetGraph;
    FAkUGCLogicNode& BudgetWaveStart = RepeatedWaveBudgetGraph.Nodes.AddDefaulted_GetRef();
    BudgetWaveStart.NodeId = FGuid::NewGuid();
    BudgetWaveStart.Type = EAkUGCLogicNodeType::WaveStart;
    for (int32 MessageIndex = 0; MessageIndex < 400; ++MessageIndex)
    {
        FAkUGCLogicNode& BudgetMessage = RepeatedWaveBudgetGraph.Nodes.AddDefaulted_GetRef();
        BudgetMessage.NodeId = FGuid::NewGuid();
        BudgetMessage.Type = EAkUGCLogicNodeType::Message;
        BudgetMessage.Message = TEXT("Budget");
        FAkUGCLogicConnection& BudgetConnection = RepeatedWaveBudgetGraph.Connections.AddDefaulted_GetRef();
        BudgetConnection.SourceNodeId = BudgetWaveStart.NodeId;
        BudgetConnection.TargetNodeId = BudgetMessage.NodeId;
    }
    TestFalse(TEXT("Three Wave Start executions share the total instruction budget"),
        FAkUGCLogicCompiler::Compile(RepeatedWaveBudgetGraph).bSucceeded);

    Document.Scenes[0].LogicGraph.Nodes.Reset();
    for (int32 NodeIndex = 0; NodeIndex <= AkUGCLogicLimits::MaxNodes; ++NodeIndex)
    {
        FAkUGCLogicNode& Node = Document.Scenes[0].LogicGraph.Nodes.AddDefaulted_GetRef();
        Node.NodeId = FGuid::NewGuid();
        Node.Type = EAkUGCLogicNodeType::Message;
        Node.Message = TEXT("Budget");
    }
    TestFalse(TEXT("Node budget is enforced before compilation"),
        FAkUGCLogicCompiler::Compile(Document.Scenes[0].LogicGraph).bSucceeded);
    return true;
}

#endif

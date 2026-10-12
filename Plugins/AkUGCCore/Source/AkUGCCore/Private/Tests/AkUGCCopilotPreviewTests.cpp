#include "Misc/AutomationTest.h"
#include "Template/AkUGCCopilotPreview.h"
#include "Prefab/AkUGCOfficialPrefabCatalog.h"
#include "Prefab/AkUGCPrefabRegistry.h"
#include "Pack/AkUGCLogicPackHasher.h"
#include "Command/AkUGCCommandExecutor.h"
#include "Pack/AkUGCLogicPackBuilder.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAkUGCCopilotPreviewTest, "AkUGC.Core.Copilot.StrictProposalPreview",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ServerContext | EAutomationTestFlags::EngineFilter)
bool FAkUGCCopilotPreviewTest::RunTest(const FString& Parameters)
{
    FAkUGCPrefabRegistry Registry;
    FAkUGCProjectDocument Document;
    FAkUGCOfficialPrefabCatalog::RegisterTowerDefense(Registry);
    FAkUGCTowerDefenseTemplate::CreateDefaultDocument(Registry, Document);
    const FString Before = FAkUGCLogicPackHasher::HashDocument(Document);
    FAkUGCTowerDefenseParameters Requested;
    Requested.Waves.SetNum(2);
    Requested.InitialResources = 300;
    Requested.Waves[0].EnemyCount = 8;
    FString Json, Error;
    FAkUGCTowerDefenseTemplate::SerializeParameters(Requested, Json);
    FAkUGCCopilotPreview Preview;
    TestTrue(TEXT("Structured proposal validates and compiles"), FAkUGCCopilotValidation::Preview(Document, Json, Preview, &Error));
    TestTrue(TEXT("Validated preview current"), FAkUGCCopilotValidation::IsCurrent(Document, Preview));
    TestEqual(TEXT("Preview has requested waves"), Preview.Parameters.Waves.Num(), 2);
    TestEqual(TEXT("Preview never mutates document"), FAkUGCLogicPackHasher::HashDocument(Document), Before);
    TestFalse(TEXT("Preview diff available"), Preview.Diff.IsEmpty());
    Document.Manifest.DisplayName += TEXT(" changed");
    TestFalse(TEXT("Changed document invalidates preview"), FAkUGCCopilotValidation::IsCurrent(Document, Preview));
    TestFalse(TEXT("Extra command field rejected"), FAkUGCCopilotValidation::Preview(Document, TEXT("{\"execute\":\"arbitrary script\"}"), Preview, &Error));
    TestFalse(TEXT("Invalid response clears prior preview"), Preview.bValidated);
    TestFalse(TEXT("Old schema not silently migrated for model output"), FAkUGCCopilotValidation::Preview(Document, Json.Replace(TEXT("\"schemaVersion\": 1"), TEXT("\"schemaVersion\": 0")), Preview, &Error));
    TestFalse(TEXT("Markdown wrapping rejected"), FAkUGCCopilotValidation::Preview(Document, TEXT("```json\n") + Json + TEXT("\n```"), Preview, &Error));
    TestFalse(TEXT("Duplicate fields rejected"), FAkUGCCopilotValidation::Preview(Document, TEXT("{\"schemaVersion\":1,\"schemaVersion\":1}"), Preview, &Error));
    TestTrue(TEXT("Explanation preserves diagnostic"), FAkUGCCopilotValidation::ExplainError(Error).Contains(Error));
    TestFalse(TEXT("Oversized proposal rejected"), FAkUGCCopilotValidation::Preview(Document, FString::ChrN(65537, TEXT('x')), Preview, &Error));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAkUGCCopilotGraphTest, "AkUGC.Core.Copilot.ControlledLogicProposal",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ServerContext | EAutomationTestFlags::EngineFilter)
bool FAkUGCCopilotGraphTest::RunTest(const FString& Parameters)
{
    FAkUGCPrefabRegistry Registry;
    FAkUGCProjectDocument Document;
    FAkUGCOfficialPrefabCatalog::RegisterTowerDefense(Registry);
    FAkUGCTowerDefenseTemplate::CreateDefaultDocument(Registry, Document);
    const FString Before = FAkUGCLogicPackHasher::HashDocument(Document);
    FAkUGCTowerDefenseParameters Proposed;
    Proposed.InitialResources = 300; Proposed.Waves.SetNum(2);
    FString Json;
    FAkUGCTowerDefenseTemplate::SerializeParameters(Proposed, Json);
    const auto* Entry = Document.Scenes[0].LogicGraph.Nodes.FindByPredicate([](const FAkUGCLogicNode& Node) { return Node.Type == EAkUGCLogicNodeType::GameStart; });
    const FString Event = Entry->NodeId.ToString(EGuidFormats::Digits);
    const FString Nodes = TEXT("[{\"id\":\"delay\",\"type\":\"Timer\",\"x\":250,\"y\":100,\"delaySeconds\":1},{\"id\":\"msg\",\"type\":\"Message\",\"x\":500,\"y\":100,\"message\":\"准备完成\"}]");
    const FString Edges = TEXT("[{\"source\":\"") + Event + TEXT("\",\"target\":\"delay\"},{\"source\":\"delay\",\"target\":\"msg\"}]");
    const auto Wrap = [&](const FString& N, const FString& E) { return TEXT("{\"schemaVersion\":1,\"parameters\":") + Json + TEXT(",\"logic\":{\"nodes\":") + N + TEXT(",\"connections\":") + E + TEXT("}}"); };
    FAkUGCCopilotPreview Preview;
    FString Error;
    TestTrue(TEXT("Controlled graph and parameters precompile"), FAkUGCCopilotValidation::Preview(Document, Wrap(Nodes, Edges), Preview, &Error));
    TestEqual(TEXT("Graph preview never mutates source"), FAkUGCLogicPackHasher::HashDocument(Document), Before);
    TestTrue(TEXT("Graph diff shows node and connection additions"), Preview.Diff.Contains(TEXT("+ 节点")) && Preview.Diff.Contains(TEXT("+ 连接")));
    FAkUGCProjectDocument Candidate = Document;
    TestTrue(TEXT("Combined transaction applies"), FAkUGCCommandExecutor::Apply(Candidate, Preview.Transaction).bSucceeded);
    TestEqual(TEXT("Two authored nodes preserved and two added"), Candidate.Scenes[0].LogicGraph.Nodes.Num(), 4);
    TestEqual(TEXT("Rules change in same transaction"), Candidate.Scenes[0].Ruleset.InitialResources, 300);
    TestTrue(TEXT("Candidate pack compiles"), FAkUGCLogicPackBuilder::Build(Candidate).bSucceeded);
    TestFalse(TEXT("Dangling graph reference rejected"), FAkUGCCopilotValidation::Preview(Document, Wrap(Nodes, Edges.Replace(TEXT("\"target\":\"msg\""), TEXT("\"target\":\"missing\""))), Preview, &Error));
    TestFalse(TEXT("Unreachable added node rejected"), FAkUGCCopilotValidation::Preview(Document, Wrap(Nodes, TEXT("[]")), Preview, &Error));
    TestFalse(TEXT("Spawn is outside graph proposal whitelist"), FAkUGCCopilotValidation::Preview(Document, Wrap(Nodes.Replace(TEXT("\"Timer\""), TEXT("\"Spawn\"")), Edges), Preview, &Error));
    TestFalse(TEXT("Extra node fields rejected"), FAkUGCCopilotValidation::Preview(Document, Wrap(Nodes.Replace(TEXT("\"delaySeconds\":1"), TEXT("\"delaySeconds\":1,\"execute\":\"script\"")), Edges), Preview, &Error));
    TestFalse(TEXT("Duplicate node id rejected"), FAkUGCCopilotValidation::Preview(Document, Wrap(Nodes.Replace(TEXT("\"id\":\"msg\""), TEXT("\"id\":\"delay\"")), Edges), Preview, &Error));
    TestFalse(TEXT("Wrong numeric type rejected"), FAkUGCCopilotValidation::Preview(Document, Wrap(Nodes.Replace(TEXT("\"x\":250"), TEXT("\"x\":\"250\"")), Edges), Preview, &Error));
    TestFalse(TEXT("Outer version string rejected"), FAkUGCCopilotValidation::Preview(Document, Wrap(Nodes, Edges).Replace(TEXT("\"schemaVersion\":1,"), TEXT("\"schemaVersion\":\"1\",")), Preview, &Error));
    const FString MissingVersion = Json.Replace(TEXT("\"schemaVersion\""), TEXT("\"removedVersion\""));
    TestFalse(TEXT("Missing inner parameters version rejected"), FAkUGCCopilotValidation::Preview(Document, Wrap(Nodes, Edges).Replace(*Json, *MissingVersion), Preview, &Error));
    TestTrue(TEXT("Missing inner version diagnostic retained"), Error.Contains(TEXT("parameters.schemaVersion")));
    TestFalse(TEXT("Numeric message rejected without string coercion"), FAkUGCCopilotValidation::Preview(Document, Wrap(Nodes.Replace(TEXT("\"message\":\"准备完成\""), TEXT("\"message\":123")), Edges), Preview, &Error));
    TestFalse(TEXT("Numeric node id rejected"), FAkUGCCopilotValidation::Preview(Document, Wrap(Nodes.Replace(TEXT("\"id\":\"msg\""), TEXT("\"id\":123")), Edges), Preview, &Error));
    const FString CycleNodes = TEXT("[{\"id\":\"a\",\"type\":\"Timer\",\"x\":0,\"y\":0,\"delaySeconds\":1},{\"id\":\"b\",\"type\":\"Timer\",\"x\":100,\"y\":0,\"delaySeconds\":1}]");
    const FString CycleEdges = TEXT("[{\"source\":\"") + Event + TEXT("\",\"target\":\"a\"},{\"source\":\"a\",\"target\":\"b\"},{\"source\":\"b\",\"target\":\"a\"}]");
    TestFalse(TEXT("Cyclic graph rejected"), FAkUGCCopilotValidation::Preview(Document, Wrap(CycleNodes, CycleEdges), Preview, &Error));
    TestTrue(TEXT("Cycle explanation categorized"), FAkUGCCopilotValidation::ExplainError(Error).Contains(TEXT("循环")));
    const FString TerminalEdges = TEXT("[{\"source\":\"") + Event + TEXT("\",\"target\":\"msg\"},{\"source\":\"msg\",\"target\":\"delay\"}]");
    TestFalse(TEXT("Message cannot have outgoing connection"), FAkUGCCopilotValidation::Preview(Document, Wrap(Nodes, TerminalEdges), Preview, &Error));
    TestFalse(TEXT("Invalid node array object rejected"), FAkUGCCopilotValidation::Preview(Document, Wrap(TEXT("[null]"), TEXT("[]")), Preview, &Error));
    TestFalse(TEXT("Invalid edge object rejected"), FAkUGCCopilotValidation::Preview(Document, Wrap(Nodes, TEXT("[null]")), Preview, &Error));
    FString TooManyNodes = TEXT("[");
    for (int32 Index = 0; Index < 33; ++Index) { TooManyNodes += (Index ? TEXT(",null") : TEXT("null")); }
    TooManyNodes += TEXT("]");
    TestFalse(TEXT("Proposal node budget enforced before parsing"), FAkUGCCopilotValidation::Preview(Document, Wrap(TooManyNodes, TEXT("[]")), Preview, &Error));
    TestTrue(TEXT("Node budget diagnostic retained"), Error.Contains(TEXT("nodes <=32")));
    FString TooManyEdges = TEXT("[");
    for (int32 Index = 0; Index < 65; ++Index) { TooManyEdges += (Index ? TEXT(",null") : TEXT("null")); }
    TooManyEdges += TEXT("]");
    TestFalse(TEXT("Proposal edge budget enforced"), FAkUGCCopilotValidation::Preview(Document, Wrap(TEXT("[]"), TooManyEdges), Preview, &Error));
    TestFalse(TEXT("Oversized message rejected"), FAkUGCCopilotValidation::Preview(Document, Wrap(Nodes.Replace(TEXT("准备完成"), *FString::ChrN(1025, TEXT('a'))), Edges), Preview, &Error));
    TestFalse(TEXT("Coordinate range enforced"), FAkUGCCopilotValidation::Preview(Document, Wrap(Nodes.Replace(TEXT("\"x\":250"), TEXT("\"x\":100001")), Edges), Preview, &Error));
    TestFalse(TEXT("Duplicate edge rejected"), FAkUGCCopilotValidation::Preview(Document, Wrap(Nodes, Edges.LeftChop(1) + TEXT(",{\"source\":\"delay\",\"target\":\"msg\"}]")), Preview, &Error));
    TestEqual(TEXT("All rejection paths preserve original"), FAkUGCLogicPackHasher::HashDocument(Document), Before);
    return true;
}
#endif

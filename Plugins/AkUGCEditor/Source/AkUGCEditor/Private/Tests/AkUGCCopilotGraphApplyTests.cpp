#include "Misc/AutomationTest.h"
#include "Editor.h"
#include "Pack/AkUGCLogicPackHasher.h"
#include "Subsystem/AkUGCEditorSubsystem.h"
#include "Template/AkUGCCopilotPreview.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAkUGCCopilotGraphApplyTest, "AkUGC.Editor.Copilot.GraphAtomicUndoAndTrust",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAkUGCCopilotGraphApplyTest::RunTest(const FString& Parameters)
{
    auto* S = GEditor->GetEditorSubsystem<UAkUGCEditorSubsystem>();
    FString Error, Json;
    if (!S || !S->NewTowerDefenseProject(&Error)) { AddError(Error); return false; }
    const FString Before = FAkUGCLogicPackHasher::HashDocument(S->GetDocument());
    const uint64 Revision = S->GetDocumentRevision();
    FAkUGCTowerDefenseParameters P;
    P.InitialResources = 300; P.Waves.SetNum(2);
    FAkUGCTowerDefenseTemplate::SerializeParameters(P, Json);
    const auto* Event = S->GetLogicGraph()->Nodes.FindByPredicate([](const FAkUGCLogicNode& N) { return N.Type == EAkUGCLogicNodeType::GameStart; });
    const FString Proposal = TEXT("{\"schemaVersion\":1,\"parameters\":") + Json
        + TEXT(",\"logic\":{\"nodes\":[{\"id\":\"msg\",\"type\":\"Message\",\"x\":300,\"y\":100,\"message\":\"开始防守\"}],\"connections\":[{\"source\":\"")
        + Event->NodeId.ToString(EGuidFormats::Digits) + TEXT("\",\"target\":\"msg\"}]}}");
    FAkUGCCopilotPreview Preview;
    if (!FAkUGCCopilotValidation::Preview(S->GetDocument(), Proposal, Preview, &Error))
    { AddError(Error); S->CloseProject(); return false; }
    auto Invalid = Preview;
    Invalid.ProposalJson = Proposal.Replace(TEXT("\"message\":\"开始防守\""), TEXT("\"message\":\"替换未确认提案\""));
    TestFalse(TEXT("Changed valid JSON without original hash is rejected"), S->ApplyCopilotProposal(Invalid, Revision, &Error));
    Invalid.ProposalJson = TEXT("{}");
    Invalid.ProposalHash = FAkUGCLogicPackHasher::Sha256Hex(Invalid.ProposalJson);
    TestFalse(TEXT("Forged validation flag and hash cannot bypass schema"), S->ApplyCopilotProposal(Invalid, Revision, &Error));
    TestEqual(TEXT("Rejected proposal leaves document"), FAkUGCLogicPackHasher::HashDocument(S->GetDocument()), Before);
    TestEqual(TEXT("Rejected proposal leaves revision"), S->GetDocumentRevision(), Revision);
    TestFalse(TEXT("Rejected proposal leaves history empty"), S->CanUndo());
    Preview.Transaction.Commands.Reset();
    Preview.Parameters.InitialResources = 999;
    TestTrue(TEXT("Original JSON is revalidated, forged payload ignored"), S->ApplyCopilotProposal(Preview, Revision, &Error));
    const FString After = FAkUGCLogicPackHasher::HashDocument(S->GetDocument());
    TestEqual(TEXT("Graph node added"), S->GetLogicGraph()->Nodes.Num(), 3);
    TestEqual(TEXT("Original resources applied"), S->GetDocument().Scenes[0].Ruleset.InitialResources, 300);
    TestFalse(TEXT("Old revision rejected"), S->ApplyCopilotProposal(Preview, Revision, &Error));
    TestTrue(TEXT("One Undo restores graph and parameters"), S->Undo().bSucceeded);
    TestEqual(TEXT("Undo exact"), FAkUGCLogicPackHasher::HashDocument(S->GetDocument()), Before);
    TestFalse(TEXT("Single history entry"), S->CanUndo());
    TestTrue(TEXT("Redo restores combination"), S->Redo().bSucceeded);
    TestEqual(TEXT("Redo exact"), FAkUGCLogicPackHasher::HashDocument(S->GetDocument()), After);
    S->CloseProject();
    return true;
}
#endif

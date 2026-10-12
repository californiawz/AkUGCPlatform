#include "Misc/AutomationTest.h"
#include "Editor.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Pack/AkUGCLogicPackHasher.h"
#include "Subsystem/AkUGCEditorSubsystem.h"
#include "Widgets/SAkUGCCopilotPanel.h"
#include "HAL/PlatformMisc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Command/AkUGCCommandExecutor.h"
#include "Document/AkUGCDocumentJson.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "ImageUtils.h"
#include "Widgets/SAkUGCCreatorPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Framework/Docking/TabManager.h"
#include "Widgets/Docking/SDockTab.h"

#if WITH_DEV_AUTOMATION_TESTS
struct FAkUGCCopilotTestAccess
{
    static void Start(SAkUGCCopilotPanel& Panel, const FString& Endpoint, float Timeout = 5.0f, const FString& Key = FString(), const FString& Ca = FString())
    {
        Panel.TestCaBundle = Ca;
        Panel.Endpoint = Endpoint; Panel.Model = TEXT("protocol-fixture-not-ai");
        Panel.Prompt = TEXT("设置两波敌人，每波8个，初始资源300。");
        Panel.ApiKey = Key; Panel.RequestTimeoutSeconds = Timeout;
        Panel.Generate();
    }
    static void StartLive(SAkUGCCopilotPanel& Panel, const FString& Prompt)
    {
        Panel.TestCaBundle.Reset();
        Panel.Endpoint = TEXT("https://api.deepseek.com/chat/completions");
        Panel.Model = TEXT("deepseek-flash");
        Panel.ApiKey = FPlatformMisc::GetEnvironmentVariable(TEXT("DEEPSEEK_API_KEY"));
        Panel.Prompt = Prompt; Panel.RequestTimeoutSeconds = 60;
        Panel.Generate();
        Panel.ApiKey.Reset();
    }
    static void Explain(SAkUGCCopilotPanel& Panel, const FString& Url, const FString& Key = FString())
    {
        Panel.Endpoint = Url; Panel.Model = Key.IsEmpty() ? TEXT("protocol-fixture-not-ai") : TEXT("deepseek-flash");
        Panel.RequestDocument = Panel.Subsystem->GetDocument(); Panel.RequestedRevision = Panel.Subsystem->GetDocumentRevision();
        FString Invalid = TEXT("{\"schemaVersion\":1,\"parameters\":{},\"logic\":{\"nodes\":[],\"connections\":[]}}");
        if (Url.EndsWith(TEXT("explain-private-field"))) { Invalid = TEXT("{\"schemaVersion\":1,\"PRIVATE_FIXTURE_MARKER_schemaVersion\":1}"); }
        if (Url.EndsWith(TEXT("explain-private-duplicate"))) { Invalid = TEXT("{\"schemaVersion\":1,\"PRIVATE_FIXTURE_MARKER\":1,\"PRIVATE_FIXTURE_MARKER\":2}"); }
        FAkUGCCopilotValidation::Preview(Panel.RequestDocument, Invalid, Panel.Preview, &Panel.LastValidationError);
        Panel.ApiKey = Key; Panel.RequestTimeoutSeconds = 60;
        Panel.Explain(); Panel.ApiKey.Reset();
    }
    static void AttemptWithoutSend(SAkUGCCopilotPanel& Panel, int32 Mode)
    {
        const FString Url = Panel.Endpoint, Prompt = Panel.Prompt;
        if (Mode == 0) { Panel.Prompt.Reset(); }
        if (Mode == 1) { Panel.Endpoint = TEXT("https://bad/endpoint?not-allowed"); }
        if (Mode == 2) { FAkUGCCopilotTransport::Shutdown(); }
        Panel.Generate();
        if (Mode == 2) { FAkUGCCopilotTransport::Initialize(); }
        Panel.Endpoint = Url; Panel.Prompt = Prompt;
    }
    static void VerifyUnsentPreserves(FAutomationTestBase* Test, SAkUGCCopilotPanel& Panel)
    {
        const auto Original = Panel.Preview;
        const FString Error = Panel.LastValidationError, Explanation = Panel.ModelExplanation;
        const FString Hash = FAkUGCLogicPackHasher::HashDocument(Panel.RequestDocument);
        const uint64 Revision = Panel.RequestedRevision;
        for (int32 Mode = 0; Mode < 3; ++Mode)
        {
            AttemptWithoutSend(Panel, Mode);
            Test->TestFalse(TEXT("Rejected start has no pending request"), Panel.Pending.IsValid());
            Test->TestEqual(TEXT("Rejected start preserves preview JSON"), Panel.Preview.ProposalJson, Original.ProposalJson);
            Test->TestEqual(TEXT("Rejected start preserves preview diff"), Panel.Preview.Diff, Original.Diff);
            Test->TestEqual(TEXT("Rejected start preserves validation flag"), Panel.Preview.bValidated, Original.bValidated);
            Test->TestEqual(TEXT("Rejected start preserves request document"), FAkUGCLogicPackHasher::HashDocument(Panel.RequestDocument), Hash);
            Test->TestEqual(TEXT("Rejected start preserves request revision"), Panel.RequestedRevision, Revision);
            Test->TestEqual(TEXT("Rejected start preserves diagnostic"), Panel.LastValidationError, Error);
            Test->TestEqual(TEXT("Rejected start preserves model advice"), Panel.ModelExplanation, Explanation);
        }
        if (Panel.CanExplain())
        {
            FAkUGCCopilotTransport::Shutdown(); Panel.Explain(); FAkUGCCopilotTransport::Initialize();
            Test->TestFalse(TEXT("Unsent explanation has no pending request"), Panel.Pending.IsValid());
            Test->TestEqual(TEXT("Unsent explanation preserves old advice"), Panel.ModelExplanation, Explanation);
            Test->TestEqual(TEXT("Unsent explanation preserves original diagnostic"), Panel.LastValidationError, Error);
        }
    }
    static uint64 Revision(const SAkUGCCopilotPanel& Panel) { return Panel.RequestedRevision; }
    static FString RequestHash(const SAkUGCCopilotPanel& Panel) { return FAkUGCLogicPackHasher::HashDocument(Panel.RequestDocument); }
    static bool KeyEmpty(const SAkUGCCopilotPanel& Panel) { return Panel.ApiKey.IsEmpty(); }
    static bool CanExplain(const SAkUGCCopilotPanel& Panel) { return Panel.CanExplain(); }
    static void RetryExplanation(SAkUGCCopilotPanel& Panel) { Panel.Explain(); }
    static const FString& Explanation(const SAkUGCCopilotPanel& Panel) { return Panel.ModelExplanation; }
    static const FString& Diagnostic(const SAkUGCCopilotPanel& Panel) { return Panel.LastValidationError; }
    static int32 HttpCode(const SAkUGCCopilotPanel& Panel) { return Panel.LastHttpCode; }
    static int32 NativeCode(const SAkUGCCopilotPanel& Panel) { return Panel.LastNativeCode; }
    static bool Pending(const SAkUGCCopilotPanel& Panel) { return Panel.Pending.IsValid(); }
    static const FAkUGCCopilotPreview& Preview(const SAkUGCCopilotPanel& Panel) { return Panel.Preview; }
    static const FString& Status(const SAkUGCCopilotPanel& Panel) { return Panel.Status; }
    static void Apply(SAkUGCCopilotPanel& Panel) { Panel.Apply(); }
    static void Cancel(SAkUGCCopilotPanel& Panel) { Panel.Cancel(); }
    static FAkUGCCopilotRequestPtr Request(const SAkUGCCopilotPanel& Panel) { return Panel.Pending; }
    static void RetiredCompletion(SAkUGCCopilotPanel& Panel, FAkUGCCopilotRequestPtr Retired)
    { Panel.Complete(Retired, {}); }
    static void Poll(SAkUGCCopilotPanel& Panel)
    { Panel.Tick(FGeometry(), FPlatformTime::Seconds(), 0.0f); }
};

namespace AkUGCCopilotHttpTest
{
    class FObserve final : public IAutomationLatentCommand
    {
    public:
        FObserve(FAutomationTestBase* InTest, const FString& InBase) : Test(InTest), Base(InBase), Start(FPlatformTime::Seconds()) {}
        bool Update() override
        {
            if (FPlatformTime::Seconds() - Start > 90)
            { Test->AddError(TEXT("Copilot HTTP fixture timed out.")); Cleanup(); return true; }
            auto* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UAkUGCEditorSubsystem>() : nullptr;
            if (!Subsystem) { Test->AddError(TEXT("Missing Creator subsystem.")); return true; }
            if (Index >= Cases.Num()) { Cleanup(); UE_LOG(LogTemp, Display, TEXT("[AkUGCCopilotHTTP] Passed Cases=%d FixtureOnly=1"), Cases.Num()); return true; }
            const FString Case = Cases[Index];
            if (bDestroyed) { return FinishDestroyed(); }
            if (!Panel.IsValid())
            {
                FString Error;
                if (!Subsystem->NewTowerDefenseProject(&Error)) { Test->AddError(Error); return true; }
                ExpectedHash = FAkUGCLogicPackHasher::HashDocument(Subsystem->GetDocument());
                Panel = SNew(SAkUGCCopilotPanel, Subsystem);
                CaseStart = FPlatformTime::Seconds();
                if (Case == TEXT("invalid-endpoints"))
                {
                    for (const FString& Url : {TEXT("http://127.0.0.1:0/ok"), TEXT("http://127.0.0.1:65536/ok"),
                        TEXT("http://127.0.0.1:18080@other.example/ok"), TEXT("http://127.0.0.1.evil/ok"),
                        TEXT("http://example.com/ok"), TEXT("https://host:abc/ok"), TEXT("https://host/ok?secret=value"),
                        TEXT("https://host/ok#fragment"), TEXT("https://host\\evil/ok"), TEXT("https://host/ok\n"), TEXT("https://-host/ok")})
                    {
                        FAkUGCCopilotTestAccess::Start(*Panel, Url);
                        Test->TestFalse(TEXT("Invalid endpoint is not sent"), FAkUGCCopilotTestAccess::Pending(*Panel));
                    }
                    FinishCase(); return false;
                }
                if (Case == TEXT("credentials-blocked"))
                {
                    FAkUGCCopilotTestAccess::Start(*Panel, Base + TEXT("/credentials-must-not-arrive"), 5, TEXT("fixture-not-a-real-key"));
                    Test->TestFalse(TEXT("Credential-bearing request fails closed"), FAkUGCCopilotTestAccess::Pending(*Panel));
                    Test->TestFalse(TEXT("Transport independently rejects HTTP credentials"), FAkUGCCopilotTransport::Start(Base + TEXT("/credentials-must-not-arrive"), TEXT("{}"), TEXT("fixture-not-a-real-key"), 5).IsValid());
                    Test->TestTrue(TEXT("Credential policy shown"), FAkUGCCopilotTestAccess::Status(*Panel).Contains(TEXT("API Key")));
                    FinishCase(); return false;
                }
                const bool Delayed = Case == TEXT("cancel-new") || Case == TEXT("cancel-only") || Case == TEXT("stale")
                    || Case == TEXT("closed") || Case == TEXT("timeout") || Case == TEXT("destroy") || Case == TEXT("shutdown-reinit");
                const FString Path = Case == TEXT("timeout") ? TEXT("/timeout-delayed") : Delayed ? TEXT("/delayed") : Case == TEXT("stale-apply") ? TEXT("/ok") : TEXT("/") + Case;
                FString Url = Base + Path, Key, Ca;
                if (Case.StartsWith(TEXT("tls-")))
                {
                    FString Tls;
                    FParse::Value(FCommandLine::Get(), TEXT("CopilotTlsFixture="), Tls);
                    FParse::Value(FCommandLine::Get(), TEXT("CopilotTlsCa="), Ca);
                    Url = Tls + TEXT("/") + Case;
                    Key = TEXT("fixture-not-a-real-key");
                    if (Case == TEXT("tls-untrusted")) { Ca.Reset(); }
                    if (Case == TEXT("tls-hostname"))
                    { FString Mismatch; FParse::Value(FCommandLine::Get(), TEXT("CopilotTlsMismatch="), Mismatch); Url = Mismatch + TEXT("/") + Case; }
                }
                if (Case.StartsWith(TEXT("explain-"))) { FAkUGCCopilotTestAccess::Explain(*Panel, Url); }
                else { FAkUGCCopilotTestAccess::Start(*Panel, Url, Case == TEXT("timeout") ? 0.25f : 5.0f, Key, Ca); }
                OriginalRequest = FAkUGCCopilotTestAccess::Request(*Panel);
                Test->TestTrue(TEXT("HTTP request started"), FAkUGCCopilotTestAccess::Pending(*Panel));
                if (Case == TEXT("stale") || Case == TEXT("explain-stale"))
                {
                    FAkUGCTowerDefenseParameters Edited;
                    Subsystem->GetTowerDefenseParameters(Edited); Edited.InitialResources = 444;
                    Test->TestTrue(TEXT("Project changed during request"), Subsystem->ApplyTowerDefenseParameters(Edited, &Error));
                    ExpectedHash = FAkUGCLogicPackHasher::HashDocument(Subsystem->GetDocument());
                }
                if (Case == TEXT("closed"))
                {
                    Subsystem->CloseProject();
                    ExpectedHash = FAkUGCLogicPackHasher::HashDocument(Subsystem->GetDocument());
                }
                return false;
            }
            if (Case == TEXT("destroy"))
            {
                if (!bDestroyed) { Panel.Reset(); bDestroyed = true; }
                return FinishDestroyed();
            }
            if ((Case == TEXT("cancel-new") || Case == TEXT("cancel-only") || Case == TEXT("explain-cancel")) && !bCancelled && FPlatformTime::Seconds() - CaseStart > 0.2)
            {
                FAkUGCCopilotTestAccess::Cancel(*Panel);
                if (Case == TEXT("cancel-new"))
                {
                    FAkUGCCopilotTestAccess::Start(*Panel, Base + TEXT("/new-result"));
                    const auto NewRequest = FAkUGCCopilotTestAccess::Request(*Panel);
                    const FString NewStatus = FAkUGCCopilotTestAccess::Status(*Panel);
                    FAkUGCCopilotTestAccess::RetiredCompletion(*Panel, OriginalRequest);
                    Test->TestTrue(TEXT("Retired completion cannot clear new pending request"), FAkUGCCopilotTestAccess::Request(*Panel) == NewRequest);
                    Test->TestEqual(TEXT("Retired completion cannot alter new pending status"), FAkUGCCopilotTestAccess::Status(*Panel), NewStatus);
                }
                bCancelled = true;
            }
            if (Case == TEXT("shutdown-reinit") && !bCancelled)
            {
                if (FPlatformTime::Seconds() - CaseStart < 0.2) { return false; }
                const double Began = FPlatformTime::Seconds();
                FAkUGCCopilotTransport::Shutdown();
                Test->TestTrue(TEXT("Shutdown joins active cancelled workers"), FPlatformTime::Seconds() - Began < 5.0);
                Test->TestTrue(TEXT("Original worker exited before cleanup completed"), OriginalRequest.IsValid() && OriginalRequest->IsDone());
                Test->TestTrue(TEXT("Transport initializes after full cleanup"), FAkUGCCopilotTransport::Initialize());
                FAkUGCCopilotTestAccess::Cancel(*Panel);
                FAkUGCCopilotTestAccess::Start(*Panel, Base + TEXT("/ok"));
                bCancelled = true;
            }
            FAkUGCCopilotTestAccess::Poll(*Panel);
            if (FAkUGCCopilotTestAccess::Pending(*Panel)) { return false; }
            if ((Case == TEXT("cancel-new") || Case == TEXT("cancel-only")) && FPlatformTime::Seconds() - CaseStart < 1.8) { return false; }
            Test->TestEqual(TEXT("HTTP generation never changes project"), FAkUGCLogicPackHasher::HashDocument(Subsystem->GetDocument()), ExpectedHash);
            if (Case.StartsWith(TEXT("explain-")))
            {
                Test->TestFalse(TEXT("Explanation never creates an applyable proposal"), FAkUGCCopilotTestAccess::Preview(*Panel).bValidated);
                if (Case == TEXT("explain-ok") || Case.StartsWith(TEXT("explain-private-")))
                {
                    Test->TestFalse(TEXT("Valid explanation displayed"), FAkUGCCopilotTestAccess::Explanation(*Panel).IsEmpty());
                    Test->TestTrue(TEXT("Raw diagnostic retained"), FAkUGCCopilotTestAccess::Status(*Panel).Contains(FAkUGCCopilotTestAccess::Diagnostic(*Panel)));
                    if (Case.StartsWith(TEXT("explain-private-")))
                    { Test->TestTrue(TEXT("Privacy marker retained only in local diagnostic"), FAkUGCCopilotTestAccess::Diagnostic(*Panel).Contains(TEXT("PRIVATE_FIXTURE_MARKER"))); }
                    FAkUGCCopilotTestAccess::VerifyUnsentPreserves(Test, *Panel);
                    Test->TestFalse(TEXT("Explanation completion does not send another request"), FAkUGCCopilotTestAccess::Pending(*Panel));
                }
                else { Test->TestTrue(TEXT("Invalid cancelled or stale explanation discarded"), FAkUGCCopilotTestAccess::Explanation(*Panel).IsEmpty()); }
                if (Case == TEXT("explain-stale"))
                { Test->TestFalse(TEXT("Old diagnostic cannot be re-explained on edited project"), FAkUGCCopilotTestAccess::CanExplain(*Panel)); }
                const bool HadUndo = Subsystem->CanUndo();
                FAkUGCCopilotTestAccess::Apply(*Panel);
                Test->TestEqual(TEXT("Apply cannot use explanation text"), FAkUGCLogicPackHasher::HashDocument(Subsystem->GetDocument()), ExpectedHash);
                Test->TestEqual(TEXT("Explanation never alters undo history"), Subsystem->CanUndo(), HadUndo);
            }
            else if (Case == TEXT("stale-apply"))
            {
                Test->TestTrue(TEXT("Proposal initially valid before edit"), FAkUGCCopilotTestAccess::Preview(*Panel).bValidated);
                FAkUGCTowerDefenseParameters Edited;
                Subsystem->GetTowerDefenseParameters(Edited); Edited.InitialResources = 444;
                Test->TestTrue(TEXT("User edit before proposal apply"), Subsystem->ApplyTowerDefenseParameters(Edited));
                const FString EditedHash = FAkUGCLogicPackHasher::HashDocument(Subsystem->GetDocument());
                FAkUGCCopilotTestAccess::Apply(*Panel);
                Test->TestEqual(TEXT("Stale proposal cannot overwrite user edit"), FAkUGCLogicPackHasher::HashDocument(Subsystem->GetDocument()), EditedHash);
                Test->TestFalse(TEXT("Stale proposal cleared"), FAkUGCCopilotTestAccess::Preview(*Panel).bValidated);
            }
            else if (Case == TEXT("ok") || Case == TEXT("cancel-new") || Case == TEXT("boundary") || Case == TEXT("tls-ok") || Case == TEXT("shutdown-reinit") || Case == TEXT("graph"))
            {
                const auto& Preview = FAkUGCCopilotTestAccess::Preview(*Panel);
                Test->TestTrue(TEXT("HTTP output validated"), Preview.bValidated);
                Test->TestEqual(TEXT("HTTP proposal contains two waves"), Preview.Parameters.Waves.Num(), 2);
                Test->TestEqual(TEXT("HTTP proposal initial resources"), Preview.Parameters.InitialResources, 300);
                if (Case == TEXT("cancel-new"))
                {
                    const FString Diff = Preview.Diff;
                    const FString Status = FAkUGCCopilotTestAccess::Status(*Panel);
                    FAkUGCCopilotTestAccess::RetiredCompletion(*Panel, OriginalRequest);
                    Test->TestEqual(TEXT("Retired completion preserves finished preview"), FAkUGCCopilotTestAccess::Preview(*Panel).Diff, Diff);
                    Test->TestEqual(TEXT("Retired completion preserves finished status"), FAkUGCCopilotTestAccess::Status(*Panel), Status);
                }
                if (Case == TEXT("ok")) { FAkUGCCopilotTestAccess::VerifyUnsentPreserves(Test, *Panel); }
                FAkUGCCopilotTestAccess::Apply(*Panel);
                Test->TestTrue(TEXT("Explicit apply changes project"), FAkUGCLogicPackHasher::HashDocument(Subsystem->GetDocument()) != ExpectedHash);
                Test->TestTrue(TEXT("HTTP proposal single undo succeeds"), Subsystem->Undo().bSucceeded);
                Test->TestEqual(TEXT("Undo restores original"), FAkUGCLogicPackHasher::HashDocument(Subsystem->GetDocument()), ExpectedHash);
                Test->TestFalse(TEXT("Only one undo entry"), Subsystem->CanUndo());
            }
            else
            {
                Test->TestFalse(TEXT("Invalid or stale HTTP response rejected"), FAkUGCCopilotTestAccess::Preview(*Panel).bValidated);
                if (Case == TEXT("oversize") || Case == TEXT("chunked") || Case == TEXT("boundary-plus-one"))
                { Test->TestTrue(TEXT("Receiving limit reported"), FAkUGCCopilotTestAccess::Status(*Panel).Contains(TEXT("256KiB"))); }
                if (Case == TEXT("stale") || Case == TEXT("closed"))
                { Test->TestTrue(TEXT("Changed revision reported"), FAkUGCCopilotTestAccess::Status(*Panel).Contains(TEXT("过期"))); }
                if (Case == TEXT("closed")) { Test->TestFalse(TEXT("Closed project stays closed"), Subsystem->HasOpenProject()); }
                if (Case == TEXT("redirect") || Case == TEXT("redirect-307") || Case.StartsWith(TEXT("tls-redirect")))
                { Test->TestTrue(TEXT("Redirect stopped before following target"), FAkUGCCopilotTestAccess::Status(*Panel).Contains(TEXT("禁止跳转"))); }
                if (Case == TEXT("graph-cycle"))
                { Test->TestTrue(TEXT("Graph cycle diagnostic categorized"), FAkUGCCopilotTestAccess::Status(*Panel).Contains(TEXT("循环"))); }
                if (Case == TEXT("graph-types"))
                { Test->TestTrue(TEXT("Graph type diagnostic preserved"), FAkUGCCopilotTestAccess::Status(*Panel).Contains(TEXT("logic.nodes.message"))); }
                if (Case == TEXT("tls-untrusted") || Case == TEXT("tls-hostname"))
                { Test->TestEqual(TEXT("Certificate chain or hostname rejected natively"), FAkUGCCopilotTestAccess::NativeCode(*Panel), 60); }
                if (Case == TEXT("timeout"))
                {
                    Test->TestEqual(TEXT("Native Curl timed out"), FAkUGCCopilotTestAccess::NativeCode(*Panel), 28);
                    Test->TestTrue(TEXT("Timeout completes before delayed service reply"), FPlatformTime::Seconds() - CaseStart < 1.5);
                    Test->TestTrue(TEXT("Timeout accurately reported"), FAkUGCCopilotTestAccess::Status(*Panel).Contains(TEXT("配置超时")));
                    UE_LOG(LogTemp, Display, TEXT("[AkUGCCopilotHTTP] NativeTimeout=1 Elapsed=%.3f"), FPlatformTime::Seconds() - CaseStart);
                }
            }
            FinishCase();
            return false;
        }
    private:
        bool FinishDestroyed()
        {
            if (FPlatformTime::Seconds() - CaseStart < 1.8) { return false; }
            auto* Subsystem = GEditor->GetEditorSubsystem<UAkUGCEditorSubsystem>();
            Test->TestEqual(TEXT("Destroyed widget request cannot mutate project"), FAkUGCLogicPackHasher::HashDocument(Subsystem->GetDocument()), ExpectedHash);
            FinishCase(); return false;
        }
        void Cleanup()
        {
            Panel.Reset();
            if (GEditor) { if (auto* Subsystem = GEditor->GetEditorSubsystem<UAkUGCEditorSubsystem>()) { Subsystem->CloseProject(); } }
        }
        void FinishCase()
        {
            UE_LOG(LogTemp, Display, TEXT("[AkUGCCopilotHTTP] Case=%s Complete=1"), *Cases[Index]);
            Panel.Reset(); ++Index; bCancelled = false; bDestroyed = false;
        }
        FAutomationTestBase* Test;
        FString Base, ExpectedHash;
        double Start, CaseStart = 0;
        int32 Index = 0;
        bool bCancelled = false, bDestroyed = false;
        TSharedPtr<SAkUGCCopilotPanel> Panel;
        FAkUGCCopilotRequestPtr OriginalRequest;
        const TArray<FString> Cases = {TEXT("invalid-endpoints"), TEXT("credentials-blocked"), TEXT("ok"), TEXT("unauthorized"),
            TEXT("malformed"), TEXT("bad-choice"), TEXT("markdown"), TEXT("extra-field"), TEXT("oversize"), TEXT("chunked"),
            TEXT("boundary"), TEXT("boundary-plus-one"), TEXT("redirect"), TEXT("redirect-307"),
            TEXT("stale"), TEXT("stale-apply"), TEXT("closed"), TEXT("timeout"), TEXT("cancel-only"), TEXT("cancel-new"), TEXT("destroy"), TEXT("tls-ok"), TEXT("tls-redirect302"), TEXT("tls-redirect307"),
            TEXT("tls-redirect-downgrade"), TEXT("tls-untrusted"), TEXT("tls-hostname"), TEXT("shutdown-reinit"), TEXT("graph"), TEXT("graph-cycle"), TEXT("graph-types"),
            TEXT("explain-ok"), TEXT("explain-types"), TEXT("explain-extra"), TEXT("explain-stale"), TEXT("explain-cancel"),
            TEXT("explain-duplicate"), TEXT("explain-long"), TEXT("outer-duplicate-choices"), TEXT("outer-duplicate-content"),
            TEXT("explain-private-field"), TEXT("explain-private-duplicate")};
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAkUGCCopilotHttpTest, "AkUGC.Editor.Copilot.RealHttpProtocol",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAkUGCCopilotHttpTest::RunTest(const FString& Parameters)
{
    FString Base;
    if (!FParse::Value(FCommandLine::Get(), TEXT("CopilotFixture="), Base))
    { AddWarning(TEXT("Requires local HTTP protocol fixture; this test is not real AI validation.")); return true; }
    if (!Base.StartsWith(TEXT("http://127.0.0.1:")) || Base.Contains(TEXT("@"))) { AddError(TEXT("Fixture must be loopback.")); return false; }
    ADD_LATENT_AUTOMATION_COMMAND(AkUGCCopilotHttpTest::FObserve(this, Base));
    return true;
}
namespace AkUGCCopilotLiveTest
{
    class FObserve final : public IAutomationLatentCommand
    {
    public:
        FObserve(FAutomationTestBase* InTest, FString InOutput) : Test(InTest), Output(MoveTemp(InOutput)) {}
        bool Update() override
        {
            auto* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UAkUGCEditorSubsystem>() : nullptr;
            if (!Subsystem) { Test->AddError(TEXT("Missing Creator subsystem.")); Finish(false); return true; }
            if (!Panel.IsValid())
            {
                FString Error;
                if (!Subsystem->NewTowerDefenseProject(&Error)) { Test->AddError(Error); Finish(false); return true; }
                Before = FAkUGCLogicPackHasher::HashDocument(Subsystem->GetDocument());
                Panel = SNew(SAkUGCCopilotPanel, Subsystem);
                Started = FPlatformTime::Seconds();
                const FString Prompt = Index == 0
                    ? TEXT("设置两波敌人，每波8个，初始资源300。其余参数保持原值，不添加Logic。")
                    : TEXT("设置两波敌人，每波8个，初始资源300，其余参数保持原值。另在现有GameStart入口后新增一个Timer，延迟1秒后连接一个Message，消息严格为Copilot live ready。只新增这两个节点和两条连接，保持现有图不变。返回parameters和logic组合提案。");
                FAkUGCCopilotTestAccess::StartLive(*Panel, Prompt);
                return false;
            }
            FAkUGCCopilotTestAccess::Poll(*Panel);
            if (FAkUGCCopilotTestAccess::Pending(*Panel) && FPlatformTime::Seconds() - Started < 70) { return false; }
            const auto Preview = FAkUGCCopilotTestAccess::Preview(*Panel);
            auto Case = MakeShared<FJsonObject>();
            Case->SetStringField(TEXT("case"), Index == 0 ? TEXT("parameters") : TEXT("controlled-logic"));
            Case->SetNumberField(TEXT("http_status"), FAkUGCCopilotTestAccess::HttpCode(*Panel));
            Case->SetNumberField(TEXT("native_code"), FAkUGCCopilotTestAccess::NativeCode(*Panel));
            Case->SetNumberField(TEXT("elapsed_seconds"), FPlatformTime::Seconds() - Started);
            Case->SetStringField(TEXT("status"), FAkUGCCopilotTestAccess::Status(*Panel));
            const bool Untouched = Before == FAkUGCLogicPackHasher::HashDocument(Subsystem->GetDocument()) && !Subsystem->CanUndo();
            bool Values = Preview.bValidated && Preview.Parameters.Waves.Num() == 2 && Preview.Parameters.InitialResources == 300;
            if (Values) { for (const auto& Wave : Preview.Parameters.Waves) { Values &= Wave.EnemyCount == 8; } }
            bool Graph = false;
            if (Values && Index == 0)
            {
                FAkUGCProjectDocument Candidate = Subsystem->GetDocument();
                const auto Result = FAkUGCCommandExecutor::Apply(Candidate, Preview.Transaction);
                Graph = Result.bSucceeded && Candidate.Scenes[0].LogicGraph.Nodes.Num() == 2
                    && Candidate.Scenes[0].LogicGraph.Connections.IsEmpty();
            }
            if (Values && Index == 1)
            {
                FAkUGCProjectDocument Candidate = Subsystem->GetDocument();
                const auto Result = FAkUGCCommandExecutor::Apply(Candidate, Preview.Transaction);
                const auto& Nodes = Candidate.Scenes[0].LogicGraph.Nodes;
                const auto& Edges = Candidate.Scenes[0].LogicGraph.Connections;
                const auto* Timer = Nodes.FindByPredicate([](const FAkUGCLogicNode& Node) { return Node.Type == EAkUGCLogicNodeType::Timer && Node.DelaySeconds == 1; });
                const auto* Message = Nodes.FindByPredicate([](const FAkUGCLogicNode& Node) { return Node.Type == EAkUGCLogicNodeType::Message && Node.Message == TEXT("Copilot live ready"); });
                Graph = Result.bSucceeded && Nodes.Num() == 4 && Edges.Num() == 2 && Timer && Message;
                if (Graph)
                {
                    const auto* Entry = Nodes.FindByPredicate([](const FAkUGCLogicNode& Node) { return Node.Type == EAkUGCLogicNodeType::GameStart; });
                    Graph = Entry && Edges.ContainsByPredicate([&](const FAkUGCLogicConnection& Edge) { return Edge.SourceNodeId == Entry->NodeId && Edge.TargetNodeId == Timer->NodeId; })
                        && Edges.ContainsByPredicate([&](const FAkUGCLogicConnection& Edge) { return Edge.SourceNodeId == Timer->NodeId && Edge.TargetNodeId == Message->NodeId; });
                }
            }
            Case->SetBoolField(TEXT("preview_validated"), Preview.bValidated);
            Case->SetBoolField(TEXT("generation_left_project_unchanged"), Untouched);
            Case->SetBoolField(TEXT("requested_values_matched"), Values);
            Case->SetBoolField(TEXT("requested_logic_matched"), Graph);
            bool Applied = false, Undo = false, Redo = false, Saved = false;
            if (Untouched && Values && Graph)
            {
                FAkUGCCopilotTestAccess::Apply(*Panel);
                const FString After = FAkUGCLogicPackHasher::HashDocument(Subsystem->GetDocument());
                Applied = After != Before && Subsystem->CanUndo();
                FString Json, ReadBack;
                FAkUGCProjectDocument Stored;
                const FString Path = FPaths::Combine(Output, Index == 0 ? TEXT("parameters.document.json") : TEXT("logic.document.json"));
                Saved = Applied && FAkUGCDocumentJson::Serialize(Subsystem->GetDocument(), Json)
                    && FFileHelper::SaveStringToFile(Json, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
                    && FFileHelper::LoadFileToString(ReadBack, *Path) && FAkUGCDocumentJson::Deserialize(ReadBack, Stored)
                    && FAkUGCLogicPackHasher::HashDocument(Stored) == After;
                Undo = Subsystem->Undo().bSucceeded && Before == FAkUGCLogicPackHasher::HashDocument(Subsystem->GetDocument()) && !Subsystem->CanUndo();
                Redo = Subsystem->Redo().bSucceeded && After == FAkUGCLogicPackHasher::HashDocument(Subsystem->GetDocument());
                Subsystem->Undo();
            }
            Case->SetBoolField(TEXT("applied_atomically"), Applied);
            Case->SetBoolField(TEXT("single_undo_exact"), Undo);
            Case->SetBoolField(TEXT("redo_exact"), Redo);
            Case->SetBoolField(TEXT("saved_document_roundtrip_exact"), Saved);
            const bool Passed = Untouched && Values && Graph && Applied && Undo && Redo && Saved;
            Case->SetBoolField(TEXT("passed"), Passed);
            Cases.Add(MakeShared<FJsonValueObject>(Case));
            if (!Passed) { Test->AddError(TEXT("Real model proposal or application validation failed; see live result.json.")); }
            Panel.Reset(); Subsystem->CloseProject();
            ++Index;
            if (!Passed || Index == 2) { Finish(Passed && Index == 2); return true; }
            return false;
        }
    private:
        void Finish(bool Passed)
        {
            Panel.Reset();
            auto Root = MakeShared<FJsonObject>();
            Root->SetStringField(TEXT("kind"), TEXT("real_deepseek_model_validation"));
            Root->SetStringField(TEXT("endpoint"), TEXT("https://api.deepseek.com/chat/completions"));
            Root->SetStringField(TEXT("model"), TEXT("deepseek-flash"));
            Root->SetBoolField(TEXT("passed"), Passed);
            Root->SetBoolField(TEXT("real_model_validated"), Passed);
            Root->SetBoolField(TEXT("fixed_response_fixture"), false);
            Root->SetArrayField(TEXT("cases"), Cases);
            FString Json;
            FJsonSerializer::Serialize(Root, TJsonWriterFactory<>::Create(&Json));
            if (!FFileHelper::SaveStringToFile(Json, *FPaths::Combine(Output, TEXT("result.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
            { Test->AddError(TEXT("Could not write real model report.")); }
        }
        FAutomationTestBase* Test;
        FString Output, Before;
        TSharedPtr<SAkUGCCopilotPanel> Panel;
        TArray<TSharedPtr<FJsonValue>> Cases;
        double Started = 0;
        int32 Index = 0;
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAkUGCCopilotLiveTest, "AkUGC.Editor.Copilot.ExplicitLiveModel",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAkUGCCopilotLiveTest::RunTest(const FString& Parameters)
{
    FString Output;
    if (!FParse::Param(FCommandLine::Get(), TEXT("CopilotLiveAuthorized")))
    { AddWarning(TEXT("Live model test skipped: explicit authorization flag required.")); return true; }
    if (FPlatformMisc::GetEnvironmentVariable(TEXT("DEEPSEEK_API_KEY")).IsEmpty()
        || !FParse::Value(FCommandLine::Get(), TEXT("CopilotLiveOutput="), Output)
        || !FPaths::IsUnderDirectory(FPaths::ConvertRelativePathToFull(Output), FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir())))
    { AddError(TEXT("Live test requires process key and output under project Saved directory.")); return false; }
    ADD_LATENT_AUTOMATION_COMMAND(AkUGCCopilotLiveTest::FObserve(this, Output));
    return true;
}
namespace AkUGCCopilotLiveExplanationTest
{
    class FObserve final : public IAutomationLatentCommand
    {
    public:
        FObserve(FAutomationTestBase* InTest, FString InOutput) : Test(InTest), Output(MoveTemp(InOutput)) {}
        bool Update() override
        {
            auto* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UAkUGCEditorSubsystem>() : nullptr;
            if (!Subsystem) { Test->AddError(TEXT("Missing Creator subsystem.")); return true; }
            if (!Panel.IsValid())
            {
                FString Error;
                if (!Subsystem->NewTowerDefenseProject(&Error)) { Test->AddError(Error); return true; }
                Before = FAkUGCLogicPackHasher::HashDocument(Subsystem->GetDocument());
                Revision = Subsystem->GetDocumentRevision();
                Panel = SNew(SAkUGCCopilotPanel, Subsystem);
                Started = FPlatformTime::Seconds();
                FAkUGCCopilotTestAccess::Explain(*Panel, TEXT("https://api.deepseek.com/chat/completions"),
                    FPlatformMisc::GetEnvironmentVariable(TEXT("DEEPSEEK_API_KEY")));
                if (FParse::Param(FCommandLine::Get(), TEXT("CopilotCapture")))
                {
                    Window = SNew(SWindow).Title(FText::FromString(TEXT("AkUGC Copilot · 真实模型只读解释验证")))
                        .ClientSize(FVector2D(1080, 760)).SupportsMaximize(false).SupportsMinimize(false)[Panel.ToSharedRef()];
                    FSlateApplication::Get().AddWindow(Window.ToSharedRef());
                }
                return false;
            }
            FAkUGCCopilotTestAccess::Poll(*Panel);
            if (FAkUGCCopilotTestAccess::Pending(*Panel) && FPlatformTime::Seconds() - Started < 70) { return false; }
            const FString Explanation = FAkUGCCopilotTestAccess::Explanation(*Panel);
            const FString Diagnostic = FAkUGCCopilotTestAccess::Diagnostic(*Panel);
            bool Captured = false;
            if (Window.IsValid())
            {
                if (ReadyAt == 0) { ReadyAt = FPlatformTime::Seconds(); return false; }
                if (FPlatformTime::Seconds() - ReadyAt < 0.5) { return false; }
                TArray<FColor> Pixels; FIntVector Size;
                if (FSlateApplication::Get().TakeScreenshot(Window.ToSharedRef(), Pixels, Size))
                {
                    TArray64<uint8> Png;
                    FImageUtils::PNGCompressImageArray(Size.X, Size.Y, TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png);
                    Captured = FFileHelper::SaveArrayToFile(Png, *FPaths::Combine(Output, TEXT("explanation.png")));
                }
            }
            const bool Unchanged = Before == FAkUGCLogicPackHasher::HashDocument(Subsystem->GetDocument())
                && Revision == Subsystem->GetDocumentRevision() && !Subsystem->CanUndo() && !FAkUGCCopilotTestAccess::Preview(*Panel).bValidated;
            FAkUGCCopilotTestAccess::Apply(*Panel);
            const bool CannotApply = Before == FAkUGCLogicPackHasher::HashDocument(Subsystem->GetDocument())
                && Revision == Subsystem->GetDocumentRevision() && !Subsystem->CanUndo();
            const bool Passed = Unchanged && CannotApply && !Explanation.IsEmpty()
                && Explanation.Contains(TEXT("schemaVersion")) && FAkUGCCopilotTestAccess::HttpCode(*Panel) == 200
                && Diagnostic == TEXT("parameters.schemaVersion must be 1.");
            const auto Report = MakeShared<FJsonObject>();
            Report->SetStringField(TEXT("kind"), TEXT("real_model_readonly_error_explanation"));
            Report->SetStringField(TEXT("model"), TEXT("deepseek-flash"));
            Report->SetBoolField(TEXT("passed"), Passed);
            Report->SetNumberField(TEXT("model_calls"), 1);
            Report->SetNumberField(TEXT("http_status"), FAkUGCCopilotTestAccess::HttpCode(*Panel));
            Report->SetNumberField(TEXT("native_code"), FAkUGCCopilotTestAccess::NativeCode(*Panel));
            Report->SetNumberField(TEXT("elapsed_seconds"), FPlatformTime::Seconds() - Started);
            Report->SetStringField(TEXT("local_validation_diagnostic"), Diagnostic);
            Report->SetStringField(TEXT("model_explanation"), Explanation);
            Report->SetBoolField(TEXT("document_revision_and_history_unchanged"), Unchanged);
            Report->SetBoolField(TEXT("explanation_cannot_apply"), CannotApply);
            Report->SetBoolField(TEXT("human_ui_validated"), false);
            Report->SetBoolField(TEXT("slate_window_captured"), Captured);
            Report->SetStringField(TEXT("screenshot"), Captured ? FPaths::Combine(Output, TEXT("explanation.png")) : FString());
            FString Json;
            FJsonSerializer::Serialize(Report, TJsonWriterFactory<>::Create(&Json));
            if (!FFileHelper::SaveStringToFile(Json, *FPaths::Combine(Output, TEXT("explanation.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
            { Test->AddError(TEXT("Cannot save live explanation report.")); }
            if (!Passed) { Test->AddError(TEXT("Real model error explanation validation failed.")); }
            if (Window.IsValid()) { FSlateApplication::Get().RequestDestroyWindow(Window.ToSharedRef()); Window.Reset(); }
            Panel.Reset(); Subsystem->CloseProject();
            return true;
        }
    private:
        FAutomationTestBase* Test;
        FString Output, Before;
        uint64 Revision = 0;
        double Started = 0, ReadyAt = 0;
        TSharedPtr<SAkUGCCopilotPanel> Panel;
        TSharedPtr<SWindow> Window;
    };
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAkUGCCopilotLiveExplanationTest, "AkUGC.Editor.Copilot.ExplicitLiveExplanation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAkUGCCopilotLiveExplanationTest::RunTest(const FString& Parameters)
{
    FString Output;
    if (!FParse::Param(FCommandLine::Get(), TEXT("CopilotLiveAuthorized")))
    { AddWarning(TEXT("Live explanation skipped: explicit authorization required.")); return true; }
    if (FPlatformMisc::GetEnvironmentVariable(TEXT("DEEPSEEK_API_KEY")).IsEmpty()
        || !FParse::Value(FCommandLine::Get(), TEXT("CopilotLiveOutput="), Output)
        || !FPaths::IsUnderDirectory(FPaths::ConvertRelativePathToFull(Output), FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir())))
    { AddError(TEXT("Live explanation requires process key and output under project Saved.")); return false; }
    ADD_LATENT_AUTOMATION_COMMAND(AkUGCCopilotLiveExplanationTest::FObserve(this, Output));
    return true;
}
namespace AkUGCCopilotMouseTest
{
    FString TextOf(const TSharedRef<SWidget>& Widget)
    {
        if (Widget->GetTypeAsString() == TEXT("STextBlock")) { return StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString(); }
        if (Widget->GetTypeAsString() == TEXT("SEditableTextBox")) { return StaticCastSharedRef<SEditableTextBox>(Widget)->GetHintText().ToString(); }
        FString Text;
        if (auto* Children = Widget->GetChildren())
        { for (int32 I = 0; I < Children->Num(); ++I) { Text += TextOf(Children->GetChildAt(I)); } }
        return Text;
    }
    class FObserve final : public IAutomationLatentCommand
    {
    public:
        FObserve(FAutomationTestBase* InTest, FString InOutput) : Test(InTest), Output(MoveTemp(InOutput)), Started(FPlatformTime::Seconds()) {}
        bool Update() override
        {
            auto* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UAkUGCEditorSubsystem>() : nullptr;
            if (!Subsystem) { Test->AddError(TEXT("Missing UI subsystem.")); return true; }
            const bool MenuMode = FParse::Param(FCommandLine::Get(), TEXT("CopilotMenuEntry"));
            const bool LiveMode = FParse::Param(FCommandLine::Get(), TEXT("CopilotMouseLive"));
            if (!Window.IsValid())
            {
                if (MenuMode)
                {
                    Window = FGlobalTabmanager::Get()->GetRootWindow();
                    if (!Window.IsValid()) { return false; }
                }
                else
                {
                    Window = SNew(SWindow).Title(FText::FromString(TEXT("AkUGC Copilot OS Mouse Verification")))
                        .ClientSize(FVector2D(1500, 940)).SupportsMaximize(false).SupportsMinimize(false)
                        [SNew(SAkUGCCreatorPanel, Subsystem)];
                    FSlateApplication::Get().AddWindow(Window.ToSharedRef());
                }
                return false;
            }
            if (FPlatformTime::Seconds() - Started > 180) { Test->AddError(TEXT("OS mouse verification timed out.")); Cleanup(); return true; }
            if (FPlatformTime::Seconds() - LastPoll < 0.15) { return false; }
            LastPoll = FPlatformTime::Seconds();
            TArray<TSharedPtr<FJsonValue>> Controls;
            TArray<TSharedRef<SWindow>> Windows = MenuMode ? FSlateApplication::Get().GetTopLevelWindows() : TArray<TSharedRef<SWindow>>{Window.ToSharedRef()};
            for (int32 I = 0; I < Windows.Num(); ++I)
            { for (const auto& Child : Windows[I]->GetChildWindows()) { Windows.AddUnique(Child); } }
            for (const auto& Top : Windows) { Walk(Top, true, Controls); }
            const auto State = MakeShared<FJsonObject>();
            State->SetBoolField(TEXT("menu_mode"), MenuMode);
            State->SetStringField(TEXT("window_title"), Window->GetTitle().ToString());
            State->SetBoolField(TEXT("creator_tab_open"), FGlobalTabmanager::Get()->FindExistingLiveTab(FTabId(TEXT("AkUGCCreator"))).IsValid());
            State->SetArrayField(TEXT("controls"), Controls);
            const FVector2D Cursor = FSlateApplication::Get().GetCursorPos();
            State->SetNumberField(TEXT("cursor_x"), Cursor.X); State->SetNumberField(TEXT("cursor_y"), Cursor.Y);
            const auto Hit = FSlateApplication::Get().LocateWindowUnderMouse(Cursor, Windows);
            TArray<TSharedPtr<FJsonValue>> Hover;
            for (int32 I = 0; I < Hit.Widgets.Num(); ++I)
            { const auto Widget = Hit.Widgets[I].Widget; Hover.Add(MakeShared<FJsonValueString>(Widget->GetTypeAsString() + TEXT(":") + TextOf(Widget))); }
            State->SetArrayField(TEXT("hover_path"), Hover);
            const auto Geometry = Window->GetWindowGeometryInScreen();
            const auto Position = Geometry.GetAbsolutePosition();
            const auto Size = Geometry.GetAbsoluteSize();
            State->SetNumberField(TEXT("window_x"), Position.X); State->SetNumberField(TEXT("window_y"), Position.Y);
            State->SetNumberField(TEXT("window_width"), Size.X); State->SetNumberField(TEXT("window_height"), Size.Y);
            State->SetBoolField(TEXT("project_open"), Subsystem->HasOpenProject());
            State->SetBoolField(TEXT("can_undo"), Subsystem->CanUndo());
            State->SetBoolField(TEXT("can_redo"), Subsystem->CanRedo());
            State->SetStringField(TEXT("document_hash"), FAkUGCLogicPackHasher::HashDocument(Subsystem->GetDocument()));
            if (Panel.IsValid())
            {
                State->SetBoolField(TEXT("pending"), FAkUGCCopilotTestAccess::Pending(*Panel));
                State->SetBoolField(TEXT("preview_validated"), FAkUGCCopilotTestAccess::Preview(*Panel).bValidated);
                State->SetStringField(TEXT("status"), FAkUGCCopilotTestAccess::Status(*Panel));
                State->SetStringField(TEXT("diff"), FAkUGCCopilotTestAccess::Preview(*Panel).Diff);
                State->SetStringField(TEXT("explanation"), FAkUGCCopilotTestAccess::Explanation(*Panel));
                State->SetNumberField(TEXT("http_code"), FAkUGCCopilotTestAccess::HttpCode(*Panel));
                State->SetBoolField(TEXT("api_key_empty"), FAkUGCCopilotTestAccess::KeyEmpty(*Panel));
            }
            Save(State, TEXT("ui-state.json"));
            FString Json;
            TSharedPtr<FJsonObject> Command;
            if (FFileHelper::LoadFileToString(Json, *FPaths::Combine(Output, TEXT("ui-command.json")))
                && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Command) && Command.IsValid())
            {
                double Seq = 0; FString Tag;
                if (Command->TryGetNumberField(TEXT("seq"), Seq) && Seq > LastSeq && Command->TryGetStringField(TEXT("tag"), Tag))
                {
                    LastSeq = Seq;
                    const FString Hash = FAkUGCLogicPackHasher::HashDocument(Subsystem->GetDocument());
                    bool Passed = false;
                    if (Tag == TEXT("menu")) { Passed = FGlobalTabmanager::Get()->FindExistingLiveTab(FTabId(TEXT("AkUGCCreator"))).IsValid() && Panel.IsValid(); }
                    if (Tag == TEXT("new")) { Before = Hash; Passed = Subsystem->HasOpenProject() && !Subsystem->CanUndo(); }
                    if (Tag == TEXT("preview"))
                    {
                        Passed = Panel.IsValid() && FAkUGCCopilotTestAccess::Preview(*Panel).bValidated && Hash == Before && !Subsystem->CanUndo();
                        if (Passed && LiveMode)
                        {
                            const auto& P = FAkUGCCopilotTestAccess::Preview(*Panel).Parameters;
                            Passed = FAkUGCCopilotTestAccess::HttpCode(*Panel) == 200 && P.Waves.Num() == 2
                                && P.InitialResources == 300 && P.Waves[0].EnemyCount == 8 && P.Waves[1].EnemyCount == 8;
                        }
                    }
                    if (Tag == TEXT("applied")) { Applied = Hash; Passed = Hash != Before && Subsystem->CanUndo() && Panel.IsValid() && !FAkUGCCopilotTestAccess::Preview(*Panel).bValidated; }
                    if (Tag == TEXT("undo")) { Passed = Hash == Before && !Subsystem->CanUndo() && Subsystem->CanRedo(); }
                    if (Tag == TEXT("redo")) { Passed = Hash == Applied && Subsystem->CanUndo(); }
                    if (Tag == TEXT("cancel")) { Passed = Panel.IsValid() && !FAkUGCCopilotTestAccess::Pending(*Panel) && !FAkUGCCopilotTestAccess::Preview(*Panel).bValidated && Hash == Before; }
                    if (Tag == TEXT("rejected")) { Passed = Panel.IsValid() && !FAkUGCCopilotTestAccess::Preview(*Panel).bValidated && FAkUGCCopilotTestAccess::CanExplain(*Panel) && Hash == Before; }
                    if (Tag == TEXT("explained")) { Passed = Panel.IsValid() && !FAkUGCCopilotTestAccess::Explanation(*Panel).IsEmpty() && !FAkUGCCopilotTestAccess::Preview(*Panel).bValidated && Hash == Before && !Subsystem->CanUndo(); }
                    if (Tag == TEXT("finish"))
                    {
                        const bool All = Checkpoints.Num() == (MenuMode ? 1 : LiveMode ? 5 : 8) && !bFailed
                            && (MenuMode || (Panel.IsValid() && FAkUGCCopilotTestAccess::KeyEmpty(*Panel)));
                        State->SetBoolField(TEXT("passed"), All); State->SetBoolField(TEXT("human_ui_validated"), false);
                        State->SetBoolField(TEXT("real_model_mode"), LiveMode);
                        State->SetNumberField(TEXT("last_http_code"), Panel.IsValid() ? FAkUGCCopilotTestAccess::HttpCode(*Panel) : 0);
                        State->SetStringField(TEXT("kind"), MenuMode ? TEXT("OS_mouse_editor_menu_entry") : LiveMode ? TEXT("OS_mouse_keyboard_real_model_generation") : TEXT("OS_mouse_keyboard_creator_copilot_fixture"));
                        State->SetArrayField(TEXT("checkpoints"), Checkpoints);
                        Save(State, TEXT("ui-result.json"));
                        if (!All) { Test->AddError(TEXT("OS UI checkpoint validation failed.")); }
                        Cleanup(); return true;
                    }
                    const TArray<FString> Order = MenuMode ? TArray<FString>{TEXT("menu")} : LiveMode
                        ? TArray<FString>{TEXT("new"), TEXT("preview"), TEXT("applied"), TEXT("undo"), TEXT("redo")}
                        : TArray<FString>{TEXT("new"), TEXT("preview"), TEXT("applied"), TEXT("undo"), TEXT("redo"), TEXT("cancel"), TEXT("rejected"), TEXT("explained")};
                    const bool Allowed = Order.Contains(Tag);
                    if (Allowed)
                    {
                        Passed &= Checkpoints.Num() < Order.Num() && Order[Checkpoints.Num()] == Tag;
                        TArray<FColor> Pixels; FIntVector ImageSize; bool Captured = false;
                        if (FSlateApplication::Get().TakeScreenshot(Window.ToSharedRef(), Pixels, ImageSize))
                        {
                            TArray64<uint8> Png;
                            FImageUtils::PNGCompressImageArray(ImageSize.X, ImageSize.Y, TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png);
                            Captured = FFileHelper::SaveArrayToFile(Png, *FPaths::Combine(Output, Tag + TEXT(".png")));
                        }
                        const auto Item = MakeShared<FJsonObject>(); Item->SetStringField(TEXT("tag"), Tag);
                        Item->SetBoolField(TEXT("passed"), Passed && Captured); Item->SetStringField(TEXT("document_hash"), Hash);
                        Checkpoints.Add(MakeShared<FJsonValueObject>(Item)); bFailed |= !Passed || !Captured;
                        State->SetNumberField(TEXT("ack"), LastSeq); State->SetBoolField(TEXT("checkpoint_passed"), Passed && Captured);
                        Save(State, TEXT("ui-ack.json"));
                    }
                }
            }
            return false;
        }
    private:
        void Walk(const TSharedRef<SWidget>& Widget, bool ParentVisible, TArray<TSharedPtr<FJsonValue>>& Controls)
        {
            const bool Visible = ParentVisible && Widget->GetVisibility().IsVisible();
            const FString Type = Widget->GetTypeAsString();
            if (Type == TEXT("SAkUGCCopilotPanel")) { Panel = StaticCastSharedRef<SAkUGCCopilotPanel>(Widget); }
            if (Visible && (Type == TEXT("SButton") || Type == TEXT("SEditableTextBox") || Type == TEXT("STextBlock")))
            {
                const auto Geometry = Widget->GetCachedGeometry(); const auto Position = Geometry.GetAbsolutePosition(); const auto Size = Geometry.GetAbsoluteSize();
                const auto Item = MakeShared<FJsonObject>(); Item->SetStringField(TEXT("type"), Type);
                Item->SetStringField(TEXT("label"), Type == TEXT("SEditableTextBox") ? StaticCastSharedRef<SEditableTextBox>(Widget)->GetHintText().ToString() : TextOf(Widget));
                Item->SetNumberField(TEXT("x"), Position.X + Size.X / 2); Item->SetNumberField(TEXT("y"), Position.Y + Size.Y / 2);
                Item->SetNumberField(TEXT("width"), Size.X); Item->SetNumberField(TEXT("height"), Size.Y);
                Item->SetBoolField(TEXT("enabled"), Widget->IsEnabled()); Controls.Add(MakeShared<FJsonValueObject>(Item));
            }
            if (auto* Children = Widget->GetChildren())
            { for (int32 I = 0; I < Children->Num(); ++I) { Walk(Children->GetChildAt(I), Visible, Controls); } }
        }
        void Save(const TSharedRef<FJsonObject>& Object, const FString& Name)
        {
            FString Json; FJsonSerializer::Serialize(Object, TJsonWriterFactory<>::Create(&Json));
            FFileHelper::SaveStringToFile(Json, *FPaths::Combine(Output, Name), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
        }
        void Cleanup()
        {
            if (!FParse::Param(FCommandLine::Get(), TEXT("CopilotMenuEntry")))
            { FSlateApplication::Get().RequestDestroyWindow(Window.ToSharedRef()); }
            Window.Reset(); Panel.Reset();
            if (GEditor) { GEditor->GetEditorSubsystem<UAkUGCEditorSubsystem>()->CloseProject(); }
        }
        FAutomationTestBase* Test;
        FString Output, Before, Applied;
        double Started, LastPoll = 0, LastSeq = 0;
        bool bFailed = false;
        TSharedPtr<SWindow> Window;
        TSharedPtr<SAkUGCCopilotPanel> Panel;
        TArray<TSharedPtr<FJsonValue>> Checkpoints;
    };
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAkUGCCopilotMouseTest, "AkUGC.Editor.Copilot.OSMouseInteraction",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAkUGCCopilotMouseTest::RunTest(const FString& Parameters)
{
    FString Output;
    if (!FParse::Value(FCommandLine::Get(), TEXT("CopilotMouseOutput="), Output))
    { AddWarning(TEXT("Mouse UI test skipped: explicit desktop driver required.")); return true; }
    if (FParse::Param(FCommandLine::Get(), TEXT("nullrhi"))
        || (FParse::Param(FCommandLine::Get(), TEXT("CopilotMouseLive")) && !FParse::Param(FCommandLine::Get(), TEXT("CopilotLiveAuthorized")))
        || !FPaths::IsUnderDirectory(FPaths::ConvertRelativePathToFull(Output), FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir())))
    { AddError(TEXT("Mouse UI requires rendering, explicit live authorization when requested, and output under Saved.")); return false; }
    ADD_LATENT_AUTOMATION_COMMAND(AkUGCCopilotMouseTest::FObserve(this, Output));
    return true;
}
#endif

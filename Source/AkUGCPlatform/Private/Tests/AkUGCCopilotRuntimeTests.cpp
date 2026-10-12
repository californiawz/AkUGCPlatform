#include "Misc/AutomationTest.h"
#include "Command/AkUGCCommandExecutor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Game/AkUGCGameMode.h"
#include "Game/AkUGCGameState.h"
#include "Prefab/AkUGCOfficialPrefabCatalog.h"
#include "Prefab/AkUGCPrefabRegistry.h"
#include "Template/AkUGCCopilotPreview.h"
#include "Document/AkUGCDocumentJson.h"
#include "Gameplay/AkUGCTowerDefenseStateHasher.h"
#include "Pack/AkUGCLogicPackBuilder.h"
#include "Pack/AkUGCLogicPackCodec.h"
#include "Pack/AkUGCLogicPackHasher.h"
#include "Pack/AkUGCLogicPackSignature.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAkUGCCopilotRuntimeTest, "AkUGC.Runtime.Copilot.ControlledGraphExecution",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ServerContext | EAutomationTestFlags::EngineFilter)
bool FAkUGCCopilotRuntimeTest::RunTest(const FString& Parameters)
{
    FAkUGCPrefabRegistry Registry;
    FAkUGCProjectDocument Document;
    FAkUGCOfficialPrefabCatalog::RegisterTowerDefense(Registry);
    FAkUGCTowerDefenseTemplate::CreateDefaultDocument(Registry, Document);
    FAkUGCTowerDefenseParameters P;
    P.Waves.SetNum(1); P.Waves[0].EnemyCount = 1; P.Waves[0].StartDelaySeconds = 2;
    FString Json, Error;
    FAkUGCTowerDefenseTemplate::SerializeParameters(P, Json);
    const auto* Event = Document.Scenes[0].LogicGraph.Nodes.FindByPredicate([](const FAkUGCLogicNode& N) { return N.Type == EAkUGCLogicNodeType::GameStart; });
    const FString Proposal = TEXT("{\"schemaVersion\":1,\"parameters\":") + Json
        + TEXT(",\"logic\":{\"nodes\":[{\"id\":\"timer\",\"type\":\"Timer\",\"x\":300,\"y\":100,\"delaySeconds\":1},")
        + TEXT("{\"id\":\"msg\",\"type\":\"Message\",\"x\":600,\"y\":100,\"message\":\"Copilot runtime message\"}],\"connections\":[{\"source\":\"")
        + Event->NodeId.ToString(EGuidFormats::Digits) + TEXT("\",\"target\":\"timer\"},{\"source\":\"timer\",\"target\":\"msg\"}]}}");
    FAkUGCCopilotPreview Preview;
    if (!FAkUGCCopilotValidation::Preview(Document, Proposal, Preview, &Error)
        || !FAkUGCCommandExecutor::Apply(Document, Preview.Transaction).bSucceeded) { AddError(Error); return false; }
    UWorld* World = NewObject<UWorld>(GetTransientPackage(), NAME_None, RF_Transient);
    World->WorldType = EWorldType::Game;
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    World->InitializeNewWorld(UWorld::InitializationValues().InitializeScenes(false).AllowAudioPlayback(false)
        .CreatePhysicsScene(false).CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false));
    auto* Mode = World->SpawnActor<AAkUGCGameMode>();
    auto* State = World->SpawnActor<AAkUGCGameState>();
    World->SetGameState(State);
    const bool Started = Mode && State && Mode->InitializeAuthoritySession(Document, &Error);
    TestTrue(TEXT("Validated combination initializes authority session"), Started);
    if (Started)
    {
        auto* Logic = World->GetSubsystem<UAkUGCLogicRuntimeSubsystem>();
        TestTrue(TEXT("First half second advances"), Logic->AdvanceLogicTime(0.5).bSucceeded);
        TestFalse(TEXT("Timer does not emit early"), Logic->GetEmittedMessages().ContainsByPredicate([](const FAkUGCLogicRuntimeMessage& M) { return M.Message == TEXT("Copilot runtime message"); }));
        TestTrue(TEXT("Timer boundary advances"), Logic->AdvanceLogicTime(0.5).bSucceeded);
        TestTrue(TEXT("Generated message executes after timer"), Logic->GetEmittedMessages().ContainsByPredicate([](const FAkUGCLogicRuntimeMessage& M) { return M.Message == TEXT("Copilot runtime message"); }));
        TestTrue(TEXT("Match continues to completion"), Logic->AdvanceLogicTime(120).bSucceeded);
        Mode->ProjectStateToGameState(State);
        TestTrue(TEXT("Rules and generated graph complete match"), State->GetScoreSnapshot().bSettled);
        TestEqual(TEXT("Generated controlled match wins"), State->GetMatchResult(), EAkUGCTowerDefenseMatchResult::Victory);
    }
    GEngine->DestroyWorldContext(World); World->DestroyWorld(false);
    return true;
}
namespace AkUGCCopilotDocumentTest
{
    struct FScopedWorld
    {
        UWorld* World;
        FScopedWorld()
        {
            World = NewObject<UWorld>(GetTransientPackage(), NAME_None, RF_Transient);
            World->WorldType = EWorldType::Game;
            GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
            World->InitializeNewWorld(UWorld::InitializationValues().InitializeScenes(false).AllowAudioPlayback(false)
                .CreatePhysicsScene(false).CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false));
        }
        ~FScopedWorld() { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); }
    };
    struct FScopedPack
    {
        FString Path = FPaths::CreateTempFilename(*FPaths::ProjectSavedDir(), TEXT("CopilotGenerated"), TEXT(".pack.json"));
        ~FScopedPack() { IFileManager::Get().Delete(*Path, false, true); }
    };
    bool Run(const FString& PackPath, const FString& PublicKey, bool SmallSteps,
        FAkUGCTowerDefenseFinalState& Final, FString& Error)
    {
        FScopedWorld Scoped;
        auto* Mode = Scoped.World->SpawnActor<AAkUGCGameMode>();
        auto* State = Scoped.World->SpawnActor<AAkUGCGameState>();
        Scoped.World->SetGameState(State);
        if (!Mode || !State || !Mode->LoadAndInitializeAuthoritySession(PackPath, PublicKey, &Error)) { return false; }
        auto* Logic = Scoped.World->GetSubsystem<UAkUGCLogicRuntimeSubsystem>();
        const auto Count = [&]()
        {
            int32 Result = 0;
            for (const auto& Message : Logic->GetEmittedMessages()) { Result += Message.Message == TEXT("Copilot live ready") ? 1 : 0; }
            return Result;
        };
        if (!Logic || Count() != 0) { Error = TEXT("Generated message emitted before timer."); return false; }
        if (SmallSteps)
        {
            if (!Logic->AdvanceLogicTime(0.5).bSucceeded || Count() != 0)
            { Error = TEXT("Generated timer emitted before one second."); return false; }
            if (!Logic->AdvanceLogicTime(0.5).bSucceeded || Count() != 1)
            { Error = TEXT("Generated message not emitted exactly once at one second."); return false; }
            for (int32 Step = 0; Step < 476; ++Step)
            {
                if (!Logic->AdvanceLogicTime(0.25).bSucceeded) { Error = TEXT("Generated small-step execution failed."); return false; }
            }
        }
        else if (!Logic->AdvanceLogicTime(120).bSucceeded)
        { Error = TEXT("Generated large-step execution failed."); return false; }
        Mode->ProjectStateToGameState(State);
        const auto Wave = State->GetWaveSnapshot();
        Final.WaveState = Wave.State; Final.MatchResult = Wave.Result;
        Final.CurrentWaveIndex = Wave.CurrentWaveIndex; Final.CurrentWaveId = Wave.CurrentWaveId;
        Final.TotalWaveCount = Wave.TotalWaveCount;
        Final.BaseCurrentHealth = State->GetBaseHealthCurrent(); Final.BaseMaximumHealth = State->GetBaseHealthMaximum();
        Final.ActiveEnemyCount = State->GetActiveEnemyCount(); Final.Score = State->GetScoreSnapshot();
        Final.Messages = Logic->GetEmittedMessages(); Final.Spawns = Logic->GetSpawnedEntities();
        Final.Goals = Logic->GetGoalReachedEntities(); Final.Damages = Logic->GetDamageEvents(); Final.Deaths = Logic->GetDeathEvents();
        if (Count() != 1 || !Final.Score.bSettled || Final.MatchResult != EAkUGCTowerDefenseMatchResult::Victory
            || Final.Spawns.Num() != 16 || Final.ActiveEnemyCount != 0 || Final.TotalWaveCount != 2)
        { Error = TEXT("Generated document message, waves or settled victory did not match."); return false; }
        return true;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAkUGCCopilotGeneratedDocumentTest, "AkUGC.Runtime.Copilot.GeneratedDocumentExecution",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ServerContext | EAutomationTestFlags::EngineFilter)
bool FAkUGCCopilotGeneratedDocumentTest::RunTest(const FString& Parameters)
{
    FString Input, Output, Error;
    if (!FParse::Value(FCommandLine::Get(), TEXT("CopilotGeneratedDocument="), Input))
    { AddWarning(TEXT("Generated document execution skipped: explicit document input required; not a model test.")); return true; }
    const FString Saved = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir());
    if (!FParse::Value(FCommandLine::Get(), TEXT("CopilotRuntimeOutput="), Output)
        || !FPaths::IsUnderDirectory(FPaths::ConvertRelativePathToFull(Input), Saved)
        || !FPaths::IsUnderDirectory(FPaths::ConvertRelativePathToFull(Output), Saved))
    { AddError(TEXT("Generated document and output must be under project Saved.")); return false; }
    auto Report = MakeShared<FJsonObject>();
    Report->SetStringField(TEXT("kind"), TEXT("generated_document_signed_runtime_execution"));
    Report->SetStringField(TEXT("input"), Input);
    Report->SetBoolField(TEXT("document_fields_modified"), false);
    Report->SetBoolField(TEXT("model_called"), false);
    const auto Execute = [&]() -> bool
    {
        const int64 Size = IFileManager::Get().FileSize(*Input);
        FString Json;
        FAkUGCProjectDocument Document;
        if (Size <= 0 || Size > 1024 * 1024 || !FFileHelper::LoadFileToString(Json, *Input)
            || !FAkUGCDocumentJson::Deserialize(Json, Document, &Error))
        { if (Error.IsEmpty()) { Error = TEXT("Generated document missing, oversized or unreadable."); } return false; }
        if (Document.Manifest.SchemaVersion != AkUGCSchema::CurrentProjectDocumentVersion || Document.Scenes.Num() != 1)
        { Error = TEXT("Generated test input must be current schema with exactly one scene."); return false; }
        const FString DocumentHash = FAkUGCLogicPackHasher::HashDocument(Document);
        Report->SetStringField(TEXT("document_hash"), DocumentHash);
        FAkUGCPrefabRegistry Registry;
        FAkUGCTowerDefenseParameters P;
        if (!FAkUGCOfficialPrefabCatalog::RegisterTowerDefense(Registry, &Error)
            || !FAkUGCTowerDefenseTemplate::ValidateDocumentBudgets(Document, Registry, &Error)
            || !FAkUGCTowerDefenseTemplate::ReadParameters(Document, Document.Scenes[0].SceneId, P, &Error)) { return false; }
        if (P.Waves.Num() != 2 || P.InitialResources != 300 || P.Waves[0].EnemyCount != 8 || P.Waves[1].EnemyCount != 8)
        { Error = TEXT("Generated parameters differ from expected request."); return false; }
        const auto Build = FAkUGCLogicPackBuilder::Build(Document);
        if (!Build.bSucceeded) { Error = Build.ErrorMessage; return false; }
        FAkUGCLogicPack Pack = Build.Pack;
        FAkUGCLogicPackKeyPair Keys;
        AkUGCCopilotDocumentTest::FScopedPack File;
        if (!FAkUGCLogicPackSigner::GenerateKeyPair(Keys, &Error)
            || !FAkUGCLogicPackSigner::Sign(Pack.Manifest, Keys.PrivateKey, Pack.Signature, &Error)
            || !FAkUGCLogicPackCodec::Serialize(Pack, Json, &Error)
            || !FFileHelper::SaveStringToFile(Json, *File.Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) { return false; }
        Keys.PrivateKey.Reset();
        FAkUGCLogicPack RoundTrip;
        if (!FAkUGCLogicPackCodec::Deserialize(Json, RoundTrip, &Error)
            || FAkUGCLogicPackHasher::HashDocument(RoundTrip.Document) != DocumentHash)
        { Error = TEXT("Signed generated document round-trip changed content."); return false; }
        Report->SetBoolField(TEXT("signed_document_roundtrip_exact"), true);
        FAkUGCTowerDefenseFinalState Large, Small;
        if (!AkUGCCopilotDocumentTest::Run(File.Path, Keys.PublicKey, false, Large, Error)
            || !AkUGCCopilotDocumentTest::Run(File.Path, Keys.PublicKey, true, Small, Error)) { return false; }
        const FString Hash = FAkUGCTowerDefenseStateHasher::HashFinalState(Large);
        const bool Equal = Hash == FAkUGCTowerDefenseStateHasher::HashFinalState(Small);
        Report->SetStringField(TEXT("final_state_hash"), Hash);
        Report->SetBoolField(TEXT("large_small_step_equal"), Equal);
        Report->SetBoolField(TEXT("verified_signature_loaded"), true);
        Report->SetBoolField(TEXT("timer_boundary_verified"), true);
        Report->SetStringField(TEXT("match_result"), TEXT("Victory"));
        Report->SetNumberField(TEXT("team_score"), Large.Score.TeamScore);
        Report->SetNumberField(TEXT("resources"), Large.Score.Resources);
        Report->SetNumberField(TEXT("enemy_kills"), Large.Score.EnemyKills);
        Report->SetNumberField(TEXT("spawn_count"), Large.Spawns.Num());
        Report->SetNumberField(TEXT("active_enemies"), Large.ActiveEnemyCount);
        if (!Equal) { Error = TEXT("Generated document final states differ by time step."); }
        return Equal;
    };
    const bool Passed = Execute();
    Report->SetBoolField(TEXT("passed"), Passed);
    Report->SetStringField(TEXT("error"), Error);
    FString Json;
    FJsonSerializer::Serialize(Report, TJsonWriterFactory<>::Create(&Json));
    if (!FFileHelper::SaveStringToFile(Json, *Output, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    { AddError(TEXT("Could not save generated runtime report.")); return false; }
    if (!Passed) { AddError(Error); }
    return Passed;
}
#endif

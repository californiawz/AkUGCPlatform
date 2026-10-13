#include "Misc/AutomationTest.h"
#include "AkUGCSceneVisualEvidence.h"

#include "Command/AkUGCCommandExecutor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Game/AkUGCGameMode.h"
#include "Game/AkUGCGameState.h"
#include "Game/AkUGCPlayerController.h"
#include "Gameplay/AkUGCTowerDefenseStateHasher.h"
#include "Pack/AkUGCLogicPackHasher.h"
#include "Prefab/AkUGCOfficialPrefabCatalog.h"
#include "Prefab/AkUGCPrefabRegistry.h"
#include "Template/AkUGCTowerDefenseBot.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "HAL/FileManager.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAkUGCSoloCycleTest,
    "AkUGC.Runtime.Solo.DeterministicBotAndReplay",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCSoloCycleTest::RunTest(const FString& Parameters)
{
    UWorld* World = NewObject<UWorld>(GetTransientPackage(), NAME_None, RF_Transient);
    World->WorldType = EWorldType::Game;
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    World->InitializeNewWorld(UWorld::InitializationValues().InitializeScenes(false).AllowAudioPlayback(false)
        .CreatePhysicsScene(false).CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false));
    AAkUGCGameMode* Mode = World->SpawnActor<AAkUGCGameMode>();
    AAkUGCGameState* State = World->SpawnActor<AAkUGCGameState>();
    World->SetGameState(State);
    FString Error;
    if (!State || !Mode->PrepareDefaultSoloProject(&Error))
    {
        AddError(TEXT("Preparation failed: ") + Error);
        GEngine->DestroyWorldContext(World); World->DestroyWorld(false); return false;
    }
    FAkUGCProjectDocument Source = Mode->GetPreparedDocument();
    Source.Manifest.ProjectId = FGuid(0x14000001, 0, 0, 0);
    FAkUGCSceneDocument& StableScene = Source.Scenes[0];
    StableScene.SceneId = FGuid(0x14000002, 0, 0, 0);
    TMap<FGuid, FGuid> StableIds;
    for (int32 Index = 0; Index < StableScene.Entities.Num(); ++Index)
    {
        StableIds.Add(StableScene.Entities[Index].EntityId, FGuid(0x14000100 + Index, 0, 0, 0));
    }
    for (auto& Entity : StableScene.Entities)
    {
        Entity.EntityId = StableIds.FindChecked(Entity.EntityId);
        if (Entity.ParentEntityId.IsValid()) { Entity.ParentEntityId = StableIds.FindChecked(Entity.ParentEntityId); }
    }
    for (int32 Index = 0; Index < StableScene.Ruleset.Waves.Num(); ++Index)
    {
        StableScene.Ruleset.Waves[Index].WaveId = FGuid(0x14000200 + Index, 0, 0, 0);
        StableScene.Ruleset.Waves[Index].SpawnPointEntityId = StableIds.FindChecked(StableScene.Ruleset.Waves[Index].SpawnPointEntityId);
    }
    if (!Mode->PrepareSoloProject(Source, &Error)) { AddError(Error); GEngine->DestroyWorldContext(World); World->DestroyWorld(false); return false; }
    const FString AuthoredHash = FAkUGCLogicPackHasher::HashDocument(Source);
    FAkUGCPrefabRegistry Registry;
    FAkUGCOfficialPrefabCatalog::RegisterTowerDefense(Registry);
    FAkUGCCommandTransaction First, Second;
    int32 FirstCount = 0, SecondCount = 0;
    TestTrue(TEXT("Bot plans first transaction"), FAkUGCTowerDefenseBot::BuildPreparationTransaction(Source, Registry, First, FirstCount, &Error));
    TestTrue(TEXT("Bot plans repeat transaction"), FAkUGCTowerDefenseBot::BuildPreparationTransaction(Source, Registry, Second, SecondCount, &Error));
    FAkUGCProjectDocument A = Source, B = Source;
    TestTrue(TEXT("Bot transaction applies"), FAkUGCCommandExecutor::Apply(A, First).bSucceeded);
    TestTrue(TEXT("Repeated bot transaction applies"), FAkUGCCommandExecutor::Apply(B, Second).bSucceeded);
    TestEqual(TEXT("Bot candidate is deterministic"), FAkUGCLogicPackHasher::HashDocument(A), FAkUGCLogicPackHasher::HashDocument(B));
    TestEqual(TEXT("Two towers are placed"), FirstCount, 2);
    TestEqual(TEXT("Resources pay for bot towers"), A.Scenes[0].Ruleset.InitialResources, 0);
    FAkUGCProjectDocument Poor = Source;
    Poor.Scenes[0].Ruleset.InitialResources = 99;
    TestTrue(TEXT("Insufficient resources is valid no-build plan"), FAkUGCTowerDefenseBot::BuildPreparationTransaction(Poor, Registry, Second, SecondCount, &Error));
    TestEqual(TEXT("Bot cannot spend unavailable resources"), SecondCount, 0);
    FAkUGCProjectDocument Overlap = Source;
    TArray<FAkUGCEntityRecord*> Slots;
    for (auto& Entity : Overlap.Scenes[0].Entities) { if (Entity.PrefabId == TEXT("official.gameplay.tower_slot")) { Slots.Add(&Entity); } }
    Slots[1]->Transform = Slots[0]->Transform;
    TestTrue(TEXT("Overlapping slots produce valid deterministic plan"), FAkUGCTowerDefenseBot::BuildPreparationTransaction(Overlap, Registry, Second, SecondCount, &Error));
    TestEqual(TEXT("Overlapping slots cannot charge for duplicate tower"), SecondCount, 1);
    FAkUGCTowerDefenseFinalState Last;
    FString FirstHash;
    for (int32 Round = 0; Round < 2; ++Round)
    {
        if (!Mode->StartSoloMatch(&Error))
        {
            AddError(TEXT("Start failed: ") + Error);
            GEngine->DestroyWorldContext(World); World->DestroyWorld(false); return false;
        }
        TestEqual(TEXT("Flow enters Playing"), State->GetMatchFlow(), EAkUGCMatchFlow::Playing);
        TestFalse(TEXT("Duplicate start rejected"), Mode->StartSoloMatch(&Error));
        TestEqual(TEXT("Bot tower count projected"), Mode->GetBotTowerCount(), 2);
        UAkUGCLogicRuntimeSubsystem* Logic = World->GetSubsystem<UAkUGCLogicRuntimeSubsystem>();
        TestTrue(TEXT("Authority simulation completes"), Logic->AdvanceLogicTime(120.0).bSucceeded);
        Mode->ProjectStateToGameState(State);
        TestEqual(TEXT("Flow enters Settlement"), State->GetMatchFlow(), EAkUGCMatchFlow::Settlement);
        TestTrue(TEXT("Match completes with immutable score"), State->GetScoreSnapshot().bSettled);
        Last.Score = State->GetScoreSnapshot();
        const auto Wave = State->GetWaveSnapshot();
        Last.WaveState = Wave.State; Last.MatchResult = Wave.Result; Last.CurrentWaveId = Wave.CurrentWaveId;
        Last.CurrentWaveIndex = Wave.CurrentWaveIndex; Last.TotalWaveCount = Wave.TotalWaveCount;
        Last.BaseCurrentHealth = State->GetBaseHealthCurrent(); Last.BaseMaximumHealth = State->GetBaseHealthMaximum();
        Last.ActiveEnemyCount = State->GetActiveEnemyCount();
        Last.Spawns = Logic->GetSpawnedEntities(); Last.Damages = Logic->GetDamageEvents();
        Last.Deaths = Logic->GetDeathEvents(); Last.Goals = Logic->GetGoalReachedEntities(); Last.Messages = Logic->GetEmittedMessages();
        const FString Hash = FAkUGCTowerDefenseStateHasher::HashFinalState(Last);
        if (Round == 0) { FirstHash = Hash; }
        else { TestEqual(TEXT("Replayed match has identical final state"), Hash, FirstHash); }
        TestEqual(TEXT("Authored document never changes"), FAkUGCLogicPackHasher::HashDocument(Mode->GetPreparedDocument()), AuthoredHash);
        TestTrue(TEXT("Return restores preparation"), Mode->ReturnToPreparation(&Error));
        TestFalse(TEXT("Return clears authority session"), Mode->HasAuthoritySession());
        TestEqual(TEXT("Return resets score"), State->GetScoreSnapshot().TeamScore, 0);
        TestEqual(TEXT("Return restores initial resources"), State->GetResources(), 200);
        TestEqual(TEXT("Return resets flow"), State->GetMatchFlow(), EAkUGCMatchFlow::Preparation);
        TestTrue(TEXT("Return cancels pending logic"), Logic->GetSpawnedEntities().IsEmpty());
    }
    FAkUGCProjectDocument External = Source;
    TestTrue(TEXT("External authority session may initialize"), Mode->InitializeAuthoritySession(External, &Error));
    TestFalse(TEXT("Solo return cannot close external session"), Mode->ReturnToPreparation(&Error));
    TestTrue(TEXT("Rejected solo return preserves external session"), Mode->HasAuthoritySession());
    UE_LOG(LogTemp, Display, TEXT("[AkUGCSolo] ReplayedHash=%s BotTowers=2 Score=%d Resources=%d"), *FirstHash, Last.Score.TeamScore, Last.Score.Resources);
    GEngine->DestroyWorldContext(World); World->DestroyWorld(false);
    return true;
}

namespace AkUGCSoloSmoke
{
    class FObserve final : public IAutomationLatentCommand
    {
    public:
        explicit FObserve(FAutomationTestBase* InTest) : Test(InTest), Started(FPlatformTime::Seconds()), Visual(TEXT("solo")) {}
        bool Update() override
        {
            if (FPlatformTime::Seconds() - Started > 180.0) { Visual.Cancel(); Test->AddError(TEXT("Solo smoke timed out.")); return true; }
            UWorld* World = nullptr;
            for (const FWorldContext& Context : GEngine->GetWorldContexts())
            {
                if (Context.World() && Context.World()->IsGameWorld() && Context.World()->GetNetMode() == NM_Standalone) { World = Context.World(); break; }
            }
            if (!World) { return false; }
            AAkUGCGameMode* Mode = World->GetAuthGameMode<AAkUGCGameMode>();
            AAkUGCGameState* State = World->GetGameState<AAkUGCGameState>();
            AAkUGCPlayerController* Controller = Cast<AAkUGCPlayerController>(World->GetFirstPlayerController());
            if (!Mode || !State || !Controller || !Mode->HasPreparedProject()) { return false; }
            if (bReturned)
            {
                if (GFrameCounter < ReturnFrame + 3) { return false; }
                if (Visual.IsEnabled() && !Visual.RecordState(World, TEXT("returned.json")))
                { Test->AddError(TEXT("Cannot persist returned camera/mesh evidence.")); return true; }
                UE_LOG(LogTemp, Display, TEXT("[AkUGCSoloSmoke] Passed Score=%d Returned=1 Resources=200"), FinalScore);
                return true;
            }
            if (!bStarted)
            {
                Controller->StartSolo();
                bStarted = true;
                if (!Mode->HasAuthoritySession()) { Test->AddError(TEXT("PlayerController start did not start solo.")); return true; }
                UE_LOG(LogTemp, Display, TEXT("[AkUGCSoloSmoke] Started BotTowers=%d"), Mode->GetBotTowerCount());
            }
            if (Visual.IsEnabled() && !bEnemyEvidence)
            { bEnemyEvidence = Visual.RecordEnemy(World); }
            if (Visual.IsEnabled())
            {
                if (!Visual.Update(World)) { return false; }
                if (!Visual.Succeeded()) { Visual.Cancel(); Test->AddError(Visual.GetError()); return true; }
            }
            if (!State->GetScoreSnapshot().bSettled) { return false; }
            if (Visual.IsEnabled() && !bEnemyEvidence)
            { Test->AddError(TEXT("No projected official enemy evidence observed.")); return true; }
            FString Capture;
            if (FParse::Value(FCommandLine::Get(), TEXT("AkUGCHUDCapture="), Capture) && IFileManager::Get().FileSize(*Capture) <= 0) { return false; }
            const int32 Score = State->GetScoreSnapshot().TeamScore;
            if (Visual.IsEnabled() && !Visual.RecordState(World, TEXT("settlement.json")))
            { Test->AddError(TEXT("Cannot persist final settlement camera/mesh evidence.")); return true; }
            Controller->ReturnSolo();
            if (Mode->HasAuthoritySession() || State->GetMatchFlow() != EAkUGCMatchFlow::Preparation || State->GetResources() != 200)
            {
                Test->AddError(TEXT("PlayerController return did not reset preparation.")); return true;
            }
            if (Visual.IsEnabled())
            { bReturned = true; ReturnFrame = GFrameCounter; FinalScore = Score; return false; }
            UE_LOG(LogTemp, Display, TEXT("[AkUGCSoloSmoke] Passed Score=%d Returned=1 Resources=200"), Score);
            return true;
        }
    private:
        FAutomationTestBase* Test;
        double Started;
        bool bStarted = false, bEnemyEvidence = false, bReturned = false;
        uint64 ReturnFrame = 0;
        int32 FinalScore = 0;
        AkUGCSceneVisual::FCapture Visual;
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAkUGCSoloSmokeTest, "AkUGC.Runtime.Solo.PlayableSmoke",
    EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCSoloSmokeTest::RunTest(const FString& Parameters)
{
    if (!FParse::Param(FCommandLine::Get(), TEXT("AkUGCSoloSmoke"))) { AddWarning(TEXT("Requires -AkUGCSoloSmoke in actual standalone game.")); return true; }
    ADD_LATENT_AUTOMATION_COMMAND(AkUGCSoloSmoke::FObserve(this));
    return true;
}

#endif

#include "Misc/AutomationTest.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Game/AkUGCGameMode.h"
#include "Game/AkUGCGameState.h"
#include "Tests/AkUGCGameStateTestListener.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AkUGCGameStateTest
{
    UWorld* CreateTestWorld()
    {
        UWorld* World = NewObject<UWorld>(GetTransientPackage(), NAME_None, RF_Transient);
        World->WorldType = EWorldType::Game;
        FWorldContext& WorldContext = GEngine->CreateNewWorldContext(World->WorldType);
        WorldContext.SetCurrentWorld(World);
        World->InitializeNewWorld(UWorld::InitializationValues()
            .InitializeScenes(false)
            .AllowAudioPlayback(false)
            .RequiresHitProxies(false)
            .CreatePhysicsScene(false)
            .CreateNavigation(false)
            .CreateAISystem(false)
            .ShouldSimulatePhysics(false)
            .EnableTraceCollision(false)
            .SetTransactional(false));
        return World;
    }

    void DestroyTestWorld(UWorld* World)
    {
        GEngine->DestroyWorldContext(World);
        World->DestroyWorld(false);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCGameStateDefaultsTest,
    "AkUGC.Runtime.Replication.GameStateDefaults",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCGameStateDefaultsTest::RunTest(const FString& Parameters)
{
    if (!GEngine)
    {
        AddError(TEXT("Engine is not available."));
        return false;
    }

    UWorld* World = AkUGCGameStateTest::CreateTestWorld();
    AAkUGCGameState* GameState = World->SpawnActor<AAkUGCGameState>();
    TestNotNull(TEXT("GameState spawns in test world"), GameState);
    if (!GameState)
    {
        AkUGCGameStateTest::DestroyTestWorld(World);
        return false;
    }

    TestEqual(TEXT("Default wave state is Inactive"),
        GameState->GetWaveSnapshot().State, EAkUGCWaveRuntimeState::Inactive);
    TestEqual(TEXT("Default match result is InProgress"),
        GameState->GetMatchResult(), EAkUGCTowerDefenseMatchResult::InProgress);
    TestEqual(TEXT("Default current wave index is INDEX_NONE"),
        GameState->GetWaveSnapshot().CurrentWaveIndex, INDEX_NONE);
    TestEqual(TEXT("Default total wave count is zero"),
        GameState->GetWaveSnapshot().TotalWaveCount, 0);
    TestEqual(TEXT("Default base health current is zero"),
        GameState->GetBaseHealthCurrent(), 0.0);
    TestEqual(TEXT("Default base health maximum is zero"),
        GameState->GetBaseHealthMaximum(), 0.0);
    TestEqual(TEXT("Default active enemy count is zero"),
        GameState->GetActiveEnemyCount(), 0);

    AkUGCGameStateTest::DestroyTestWorld(World);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCGameStateProjectionTest,
    "AkUGC.Runtime.Replication.GameStateProjection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCGameStateProjectionTest::RunTest(const FString& Parameters)
{
    if (!GEngine)
    {
        AddError(TEXT("Engine is not available."));
        return false;
    }

    UWorld* World = AkUGCGameStateTest::CreateTestWorld();
    AAkUGCGameState* GameState = World->SpawnActor<AAkUGCGameState>();
    TestNotNull(TEXT("GameState spawns in test world"), GameState);
    if (!GameState)
    {
        AkUGCGameStateTest::DestroyTestWorld(World);
        return false;
    }

    FAkUGCWaveRuntimeSnapshot Snapshot;
    Snapshot.State = EAkUGCWaveRuntimeState::Spawning;
    Snapshot.Result = EAkUGCTowerDefenseMatchResult::InProgress;
    Snapshot.CurrentWaveIndex = 0;
    Snapshot.CurrentWaveId = FGuid::NewGuid();
    Snapshot.TotalWaveCount = 3;
    Snapshot.SecondsUntilNextBoundary = 12.5;

    GameState->ProjectWaveSnapshot(Snapshot);

    TestEqual(TEXT("Projected wave state is observable"),
        GameState->GetWaveSnapshot().State, EAkUGCWaveRuntimeState::Spawning);
    TestEqual(TEXT("Projected current wave index is observable"),
        GameState->GetWaveSnapshot().CurrentWaveIndex, 0);
    TestEqual(TEXT("Projected wave id is observable"),
        GameState->GetWaveSnapshot().CurrentWaveId, Snapshot.CurrentWaveId);
    TestEqual(TEXT("Projected total wave count is observable"),
        GameState->GetWaveSnapshot().TotalWaveCount, 3);
    TestEqual(TEXT("Projected boundary seconds is observable"),
        GameState->GetWaveSnapshot().SecondsUntilNextBoundary, 12.5);

    GameState->ProjectBaseHealth(30.0, 100.0);
    TestEqual(TEXT("Projected base health current is observable"),
        GameState->GetBaseHealthCurrent(), 30.0);
    TestEqual(TEXT("Projected base health maximum is observable"),
        GameState->GetBaseHealthMaximum(), 100.0);

    GameState->ProjectActiveEnemyCount(5);
    TestEqual(TEXT("Projected active enemy count is observable"),
        GameState->GetActiveEnemyCount(), 5);

    AkUGCGameStateTest::DestroyTestWorld(World);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCGameStateMatchEndedBroadcastTest,
    "AkUGC.Runtime.Replication.GameStateMatchEndedBroadcast",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCGameStateMatchEndedBroadcastTest::RunTest(const FString& Parameters)
{
    if (!GEngine)
    {
        AddError(TEXT("Engine is not available."));
        return false;
    }

    UWorld* World = AkUGCGameStateTest::CreateTestWorld();
    AAkUGCGameState* GameState = World->SpawnActor<AAkUGCGameState>();
    TestNotNull(TEXT("GameState spawns in test world"), GameState);
    if (!GameState)
    {
        AkUGCGameStateTest::DestroyTestWorld(World);
        return false;
    }

    UAkUGCMatchEndedTestListener* Listener = NewObject<UAkUGCMatchEndedTestListener>();
    GameState->OnMatchEndedReplicated.AddDynamic(Listener, &UAkUGCMatchEndedTestListener::OnMatchEnded);

    FAkUGCWaveRuntimeSnapshot Victory;
    Victory.State = EAkUGCWaveRuntimeState::Completed;
    Victory.Result = EAkUGCTowerDefenseMatchResult::Victory;
    Victory.CurrentWaveIndex = 2;
    Victory.TotalWaveCount = 3;

    GameState->ProjectWaveSnapshot(Victory);
    GameState->OnRep_WaveSnapshot();
    TestEqual(TEXT("Match ended broadcast fires once"), Listener->BroadcastCount, 1);
    TestEqual(TEXT("Broadcast carries victory result"),
        Listener->LastResult, EAkUGCTowerDefenseMatchResult::Victory);

    GameState->OnRep_WaveSnapshot();
    TestEqual(TEXT("Match ended broadcast does not repeat"),
        Listener->BroadcastCount, 1);

    GameState->OnMatchEndedReplicated.RemoveDynamic(Listener, &UAkUGCMatchEndedTestListener::OnMatchEnded);
    AkUGCGameStateTest::DestroyTestWorld(World);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCGameModeDefaultsTest,
    "AkUGC.Runtime.Replication.GameModeDefaults",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCGameModeDefaultsTest::RunTest(const FString& Parameters)
{
    AAkUGCGameMode* GameMode = NewObject<AAkUGCGameMode>();
    TestNotNull(TEXT("GameMode constructs"), GameMode);
    if (!GameMode)
    {
        return false;
    }

    TestTrue(TEXT("GameMode binds UGC GameState as default"),
        GameMode->GameStateClass == AAkUGCGameState::StaticClass());

    return true;
}

#endif

#include "Misc/AutomationTest.h"
#include "Gameplay/AkUGCTowerDefenseScore.h"
#include "Gameplay/AkUGCTowerDefenseStateHasher.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAkUGCScoreIdempotenceTest,
    "AkUGC.Runtime.Score.AuthorityIdempotence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
        | EAutomationTestFlags::ServerContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCScoreIdempotenceTest::RunTest(const FString& Parameters)
{
    const FGuid Enemy(1, 0, 0, 0), Base(2, 0, 0, 0), Tower(3, 0, 0, 0), Escaped(4, 0, 0, 0);
    TArray<FAkUGCLogicRuntimeSpawn> Spawns;
    Spawns.Add({FGuid(10, 0, 0, 0), Enemy, TEXT("official.unit.basic_enemy")});
    Spawns.Add({FGuid(10, 0, 0, 0), Escaped, TEXT("official.unit.basic_enemy")});
    TArray<FAkUGCLogicRuntimeDeath> Deaths;
    Deaths.Add({Tower, Enemy});
    Deaths.Add({Tower, Enemy});
    Deaths.Add({Enemy, Base});
    FAkUGCTowerDefenseScoreAccumulator Score;
    Score.Initialize(1000000);
    Score.Update(Spawns, Deaths, EAkUGCTowerDefenseMatchResult::InProgress, 90.0, 100.0);
    TestEqual(TEXT("Unique enemy kill adds 100 score"), Score.GetSnapshot().TeamScore, 100);
    TestEqual(TEXT("Duplicate death and base death do not count"), Score.GetSnapshot().EnemyKills, 1);
    TestEqual(TEXT("Resources can exceed initial resource limit via earned rewards"), Score.GetSnapshot().Resources, 1000010);
    Score.Update(Spawns, Deaths, EAkUGCTowerDefenseMatchResult::Victory, 90.0, 100.0);
    TestEqual(TEXT("Victory bonus adds 500 once"), Score.GetSnapshot().TeamScore, 600);
    Score.Update(Spawns, Deaths, EAkUGCTowerDefenseMatchResult::Victory, 0.0, 100.0);
    TestEqual(TEXT("Repeated terminal projection does not reward twice"), Score.GetSnapshot().TeamScore, 600);
    TestEqual(TEXT("Settlement freezes health evaluation"), Score.GetSnapshot().FinalBaseCurrentHealth, 90.0);
    TestTrue(TEXT("Settlement complete"), Score.GetSnapshot().bSettled);
    Score.Initialize(200);
    Score.Update(Spawns, {}, EAkUGCTowerDefenseMatchResult::Victory, 93.0, 100.0);
    TestEqual(TEXT("Escaped or merely spawned enemies are not kills"), Score.GetSnapshot().EnemyKills, 0);
    TestEqual(TEXT("Victory without kills only adds bonus"), Score.GetSnapshot().TeamScore, 500);
    Score.Initialize(200);
    Score.Update(Spawns, {{Enemy, Base}}, EAkUGCTowerDefenseMatchResult::Defeat, 0.0, 100.0);
    TestEqual(TEXT("Base death defeat has no score"), Score.GetSnapshot().TeamScore, 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAkUGCScoreHashCoverageTest,
    "AkUGC.Runtime.Score.FinalHashCoverage",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
        | EAutomationTestFlags::ServerContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCScoreHashCoverageTest::RunTest(const FString& Parameters)
{
    FAkUGCTowerDefenseFinalState State;
    const FString Baseline = FAkUGCTowerDefenseStateHasher::HashFinalState(State);
    const auto Verify = [&](const FAkUGCTowerDefenseScoreSnapshot& Score)
    {
        State.Score = Score;
        TestNotEqual(TEXT("Every score field affects final hash"), FAkUGCTowerDefenseStateHasher::HashFinalState(State), Baseline);
    };
    FAkUGCTowerDefenseScoreSnapshot Score;
    Score.TeamScore = 100; Verify(Score);
    Score = {}; Score.Resources = 10; Verify(Score);
    Score = {}; Score.EnemyKills = 1; Verify(Score);
    Score = {}; Score.bSettled = true; Verify(Score);
    Score = {}; Score.Result = EAkUGCTowerDefenseMatchResult::Victory; Verify(Score);
    Score = {}; Score.FinalBaseCurrentHealth = 90.0; Verify(Score);
    Score = {}; Score.FinalBaseMaximumHealth = 100.0; Verify(Score);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAkUGCScoreAtomicReplicationTest,
    "AkUGC.Runtime.Score.AtomicSnapshotSerialization",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
        | EAutomationTestFlags::ServerContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCScoreAtomicReplicationTest::RunTest(const FString& Parameters)
{
    FAkUGCTowerDefenseScoreSnapshot Source;
    Source.TeamScore = 800;
    Source.Resources = 230;
    Source.EnemyKills = 3;
    Source.bSettled = true;
    Source.Result = EAkUGCTowerDefenseMatchResult::Victory;
    Source.FinalBaseCurrentHealth = 99.0;
    Source.FinalBaseMaximumHealth = 100.0;
    TArray<uint8> Bytes;
    bool Success = false;
    FMemoryWriter Writer(Bytes);
    Source.NetSerialize(Writer, nullptr, Success);
    TestTrue(TEXT("Whole settlement serializes"), Success);
    FAkUGCTowerDefenseScoreSnapshot Target;
    Target.Resources = 200;
    FMemoryReader Reader(Bytes);
    Target.NetSerialize(Reader, nullptr, Success);
    TestTrue(TEXT("Whole settlement decodes atomically"), Success);
    TestEqual(TEXT("Score arrives with resources"), Target.TeamScore, 800);
    TestEqual(TEXT("Resources arrive with settlement flag"), Target.Resources, 230);
    TestEqual(TEXT("Kills arrive in same snapshot"), Target.EnemyKills, 3);
    TestTrue(TEXT("Settlement arrives complete"), Target.bSettled);
    return true;
}

#endif

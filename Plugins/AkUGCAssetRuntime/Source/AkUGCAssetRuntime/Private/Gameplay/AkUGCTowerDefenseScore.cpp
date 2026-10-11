#include "Gameplay/AkUGCTowerDefenseScore.h"

bool FAkUGCTowerDefenseScoreSnapshot::NetSerialize(FArchive& Archive, UPackageMap* Map, bool& bOutSuccess)
{
    FAkUGCTowerDefenseScoreSnapshot Value = *this;
    uint8 Settled = Value.bSettled ? 1 : 0;
    uint8 MatchResult = static_cast<uint8>(Value.Result);
    Archive << Value.RoundId << Value.TeamScore << Value.Resources << Value.EnemyKills;
    Archive.SerializeBits(&Settled, 1);
    Archive << MatchResult << Value.FinalBaseCurrentHealth << Value.FinalBaseMaximumHealth;
    Value.bSettled = Settled != 0;
    Value.Result = static_cast<EAkUGCTowerDefenseMatchResult>(MatchResult);
    bOutSuccess = !Archive.IsError() && Value.TeamScore >= 0 && Value.TeamScore <= AkUGCScoreRules::MaxScore
        && Value.Resources >= 0 && Value.Resources <= AkUGCScoreRules::MaxResources
        && Value.EnemyKills >= 0 && Value.EnemyKills <= AkUGCTowerDefenseRulesetLimits::MaxTotalEnemyCount
        && FMath::IsFinite(Value.FinalBaseCurrentHealth) && FMath::IsFinite(Value.FinalBaseMaximumHealth)
        && Value.FinalBaseCurrentHealth >= 0.0 && Value.FinalBaseMaximumHealth >= Value.FinalBaseCurrentHealth
        && MatchResult <= static_cast<uint8>(EAkUGCTowerDefenseMatchResult::Defeat)
        && (Value.bSettled ? Value.Result != EAkUGCTowerDefenseMatchResult::InProgress && Value.FinalBaseMaximumHealth > 0.0
            : Value.Result == EAkUGCTowerDefenseMatchResult::InProgress);
    if (Archive.IsLoading() && bOutSuccess) { *this = Value; }
    return true;
}

void FAkUGCTowerDefenseScoreAccumulator::Initialize(int32 InitialResources)
{
    Snapshot = FAkUGCTowerDefenseScoreSnapshot();
    Snapshot.Resources = FMath::Clamp(InitialResources, 0, AkUGCTowerDefenseRulesetLimits::MaxInitialResources);
    EnemyIds.Reset();
    RewardedEnemies.Reset();
    ReadSpawns = 0;
    ReadDeaths = 0;
}

void FAkUGCTowerDefenseScoreAccumulator::Update(
    const TArray<FAkUGCLogicRuntimeSpawn>& Spawns, const TArray<FAkUGCLogicRuntimeDeath>& Deaths,
    EAkUGCTowerDefenseMatchResult Result, double BaseCurrentHealth, double BaseMaximumHealth)
{
    if (Snapshot.bSettled) { return; }
    if (ReadSpawns > Spawns.Num() || ReadDeaths > Deaths.Num()) { return; }
    for (; ReadSpawns < Spawns.Num(); ++ReadSpawns)
    {
        const FAkUGCLogicRuntimeSpawn& Spawn = Spawns[ReadSpawns];
        if (Spawn.EntityId.IsValid() && Spawn.PrefabId == TEXT("official.unit.basic_enemy"))
        {
            EnemyIds.Add(Spawn.EntityId);
        }
    }
    for (; ReadDeaths < Deaths.Num(); ++ReadDeaths)
    {
        const FGuid& Id = Deaths[ReadDeaths].EntityId;
        if (EnemyIds.Contains(Id) && !RewardedEnemies.Contains(Id)
            && Snapshot.EnemyKills < AkUGCTowerDefenseRulesetLimits::MaxTotalEnemyCount)
        {
            RewardedEnemies.Add(Id);
            ++Snapshot.EnemyKills;
            Snapshot.TeamScore += AkUGCScoreRules::KillScore;
            Snapshot.Resources += AkUGCScoreRules::KillResources;
        }
    }
    if ((Result == EAkUGCTowerDefenseMatchResult::Victory || Result == EAkUGCTowerDefenseMatchResult::Defeat)
        && FMath::IsFinite(BaseCurrentHealth) && FMath::IsFinite(BaseMaximumHealth)
        && BaseMaximumHealth > 0.0 && BaseCurrentHealth >= 0.0 && BaseCurrentHealth <= BaseMaximumHealth)
    {
        if (Result == EAkUGCTowerDefenseMatchResult::Victory) { Snapshot.TeamScore += AkUGCScoreRules::VictoryBonus; }
        Snapshot.Result = Result;
        Snapshot.FinalBaseCurrentHealth = BaseCurrentHealth;
        Snapshot.FinalBaseMaximumHealth = BaseMaximumHealth;
        Snapshot.bSettled = true;
    }
}

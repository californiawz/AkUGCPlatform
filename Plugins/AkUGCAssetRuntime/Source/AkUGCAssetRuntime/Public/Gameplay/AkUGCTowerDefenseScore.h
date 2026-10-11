#pragma once

#include "CoreMinimal.h"
#include "Subsystem/AkUGCLogicRuntimeSubsystem.h"
#include "AkUGCTowerDefenseScore.generated.h"

namespace AkUGCScoreRules
{
    inline constexpr int32 KillScore = 100;
    inline constexpr int32 KillResources = 10;
    inline constexpr int32 VictoryBonus = 500;
    inline constexpr int32 MaxScore = AkUGCTowerDefenseRulesetLimits::MaxTotalEnemyCount * KillScore + VictoryBonus;
    inline constexpr int32 MaxResources = AkUGCTowerDefenseRulesetLimits::MaxInitialResources
        + AkUGCTowerDefenseRulesetLimits::MaxTotalEnemyCount * KillResources;
}

USTRUCT(BlueprintType)
struct AKUGCASSETRUNTIME_API FAkUGCTowerDefenseScoreSnapshot
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Score")
    int32 TeamScore = 0;

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Score")
    int32 RoundId = 0;

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Score")
    int32 Resources = 0;

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Score")
    int32 EnemyKills = 0;

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Score")
    bool bSettled = false;

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Score")
    EAkUGCTowerDefenseMatchResult Result = EAkUGCTowerDefenseMatchResult::InProgress;

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Score")
    double FinalBaseCurrentHealth = 0.0;

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Score")
    double FinalBaseMaximumHealth = 0.0;

    bool NetSerialize(FArchive& Archive, UPackageMap* Map, bool& bOutSuccess);
};

template<>
struct TStructOpsTypeTraits<FAkUGCTowerDefenseScoreSnapshot> : TStructOpsTypeTraitsBase2<FAkUGCTowerDefenseScoreSnapshot>
{
    enum { WithNetSerializer = true };
};

class AKUGCASSETRUNTIME_API FAkUGCTowerDefenseScoreAccumulator
{
public:
    void Initialize(int32 InitialResources);
    void Update(const TArray<FAkUGCLogicRuntimeSpawn>& Spawns,
        const TArray<FAkUGCLogicRuntimeDeath>& Deaths, EAkUGCTowerDefenseMatchResult Result,
        double BaseCurrentHealth, double BaseMaximumHealth);
    const FAkUGCTowerDefenseScoreSnapshot& GetSnapshot() const { return Snapshot; }

private:
    FAkUGCTowerDefenseScoreSnapshot Snapshot;
    TSet<FGuid> EnemyIds;
    TSet<FGuid> RewardedEnemies;
    int32 ReadSpawns = 0;
    int32 ReadDeaths = 0;
};

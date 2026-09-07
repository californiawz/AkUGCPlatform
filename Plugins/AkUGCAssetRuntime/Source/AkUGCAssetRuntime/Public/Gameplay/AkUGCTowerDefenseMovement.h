#pragma once

#include "CoreMinimal.h"
#include "Document/AkUGCDocument.h"

struct AKUGCASSETRUNTIME_API FAkUGCTowerDefenseEnemyMovement
{
    FGuid SourceNodeId;
    FGuid EntityId;
    double MoveSpeed = 0.0;
    double GoalDamage = 0.0;
    int32 NextPathNodeIndex = 0;
};

struct AKUGCASSETRUNTIME_API FAkUGCTowerDefenseBasicTowerAttack
{
    FGuid EntityId;
    double AttackRange = 0.0;
    double AttackInterval = 0.0;
    double AttackDamage = 0.0;
    double RemainingAttackSeconds = 0.0;
};

struct AKUGCASSETRUNTIME_API FAkUGCRuntimeHealth
{
    double Maximum = 0.0;
    double Current = 0.0;
};

struct AKUGCASSETRUNTIME_API FAkUGCRuntimeDamage
{
    FGuid SourceEntityId;
    FGuid TargetEntityId;
    double RequestedDamage = 0.0;
    double AppliedDamage = 0.0;
    double HealthAfterDamage = 0.0;
    bool bKilled = false;
};

struct AKUGCASSETRUNTIME_API FAkUGCTowerDefenseGoalReached
{
    FGuid SourceNodeId;
    FGuid EntityId;
    FGuid GoalEntityId;
    FGuid BaseEntityId;
    double DamageApplied = 0.0;
    double BaseHealthAfterDamage = 0.0;
};

struct AKUGCASSETRUNTIME_API FAkUGCTowerDefenseGameplayEvents
{
    TArray<FAkUGCRuntimeDamage> DamageEvents;
    TArray<FAkUGCTowerDefenseGoalReached> GoalReachedEvents;
};

struct AKUGCASSETRUNTIME_API FAkUGCTowerDefenseWaveRuntimeConfig
{
    FGuid WaveId;
    FGuid SpawnPointEntityId;
    FName EnemyPrefabId;
    int32 EnemyCount = 0;
    double SpawnIntervalSeconds = 0.0;
    double StartDelaySeconds = 0.0;
};

struct AKUGCASSETRUNTIME_API FAkUGCTowerDefenseRulesetRuntimeConfig
{
    TArray<FAkUGCTowerDefenseWaveRuntimeConfig> Waves;
    FGuid BaseEntityId;
    double WaveIntervalSeconds = 0.0;
    EAkUGCTowerDefenseDefeatCondition DefeatCondition = EAkUGCTowerDefenseDefeatCondition::BaseHealthDepleted;
    EAkUGCTowerDefenseVictoryCondition VictoryCondition = EAkUGCTowerDefenseVictoryCondition::AllWavesCleared;
};

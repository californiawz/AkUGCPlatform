#pragma once

#include "CoreMinimal.h"

struct AKUGCASSETRUNTIME_API FAkUGCTowerDefenseEnemyMovement
{
    FGuid SourceNodeId;
    FGuid EntityId;
    double MoveSpeed = 0.0;
    double GoalDamage = 0.0;
    int32 NextPathNodeIndex = 0;
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

struct AKUGCASSETRUNTIME_API FAkUGCTowerDefenseRuntimeHealth
{
    double Maximum = 0.0;
    double Current = 0.0;
};

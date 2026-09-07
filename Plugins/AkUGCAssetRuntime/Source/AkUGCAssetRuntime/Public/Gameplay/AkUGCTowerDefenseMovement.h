#pragma once

#include "CoreMinimal.h"

struct AKUGCASSETRUNTIME_API FAkUGCTowerDefenseEnemyMovement
{
    FGuid SourceNodeId;
    FGuid EntityId;
    double MoveSpeed = 0.0;
    int32 NextPathNodeIndex = 0;
};

struct AKUGCASSETRUNTIME_API FAkUGCTowerDefenseGoalReached
{
    FGuid SourceNodeId;
    FGuid EntityId;
};

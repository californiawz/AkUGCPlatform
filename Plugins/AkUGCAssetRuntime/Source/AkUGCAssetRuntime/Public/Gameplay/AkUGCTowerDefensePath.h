#pragma once

#include "CoreMinimal.h"

struct FAkUGCSceneDocument;

struct AKUGCASSETRUNTIME_API FAkUGCTowerDefensePathNode
{
    FGuid EntityId;
    int64 Order = 0;
    FVector Location = FVector::ZeroVector;
};

struct AKUGCASSETRUNTIME_API FAkUGCTowerDefensePath
{
    TArray<FAkUGCTowerDefensePathNode> Nodes;

    bool IsEmpty() const;
    int32 Num() const;
    const FAkUGCTowerDefensePathNode* GetNode(int32 Index) const;
};

struct AKUGCASSETRUNTIME_API FAkUGCTowerDefensePathBuildResult
{
    bool bSucceeded = false;
    bool bPathPresent = false;
    FAkUGCTowerDefensePath Path;
    FString ErrorPath;
    FString ErrorMessage;
};

class AKUGCASSETRUNTIME_API FAkUGCTowerDefensePathBuilder
{
public:
    static FAkUGCTowerDefensePathBuildResult Build(
        const FAkUGCSceneDocument& Scene,
        bool bRequireUsablePath = false);
};

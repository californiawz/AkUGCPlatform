#pragma once

#include "Command/AkUGCCommand.h"

class FAkUGCPrefabRegistry;

class AKUGCCORE_API FAkUGCTowerDefenseBot
{
public:
    static constexpr int32 TowerCost = 100;
    static constexpr int32 MaxBotTowers = 2;
    static bool BuildPreparationTransaction(const FAkUGCProjectDocument& Document,
        const FAkUGCPrefabRegistry& Registry, FAkUGCCommandTransaction& OutTransaction,
        int32& OutTowerCount, FString* OutError = nullptr);
};

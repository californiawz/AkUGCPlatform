#pragma once

#include "CoreMinimal.h"

struct FAkUGCPrefabDefinition;
class FAkUGCPrefabRegistry;

class AKUGCCORE_API FAkUGCOfficialPrefabCatalog
{
public:
    static TArray<FAkUGCPrefabDefinition> BuildTowerDefenseDefinitions();
    static TArray<FName> GetTowerDefensePrefabIds();
    static bool RegisterTowerDefense(FAkUGCPrefabRegistry& Registry, FString* OutError = nullptr);
};

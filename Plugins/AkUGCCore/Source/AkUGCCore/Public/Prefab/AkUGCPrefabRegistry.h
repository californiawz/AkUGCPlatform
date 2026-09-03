#pragma once

#include "CoreMinimal.h"
#include "Prefab/AkUGCPrefabDefinition.h"

class AKUGCCORE_API FAkUGCPrefabRegistry
{
public:
    bool Register(const FAkUGCPrefabDefinition& Definition, FString* OutError = nullptr);
    bool RegisterBatch(const TArray<FAkUGCPrefabDefinition>& NewDefinitions, FString* OutError = nullptr);
    bool Unregister(FName PrefabId);
    void Reset();

    const FAkUGCPrefabDefinition* Find(FName PrefabId) const;
    const FSoftObjectPath* ResolveAsset(FName PrefabId, FName PlatformVariant) const;
    bool CreateEntityRecord(
        FName PrefabId,
        FGuid EntityId,
        const FTransform& Transform,
        FAkUGCEntityRecord& OutEntity,
        FString* OutError = nullptr) const;
    TArray<FName> GetRegisteredIds() const;

    static bool ValidateDefinition(const FAkUGCPrefabDefinition& Definition, FString* OutError = nullptr);

private:
    TMap<FName, FAkUGCPrefabDefinition> Definitions;
};

#include "Prefab/AkUGCPrefabRegistry.h"

namespace
{
    bool Fail(FString* OutError, const FString& Message)
    {
        if (OutError)
        {
            *OutError = Message;
        }
        return false;
    }
}

bool FAkUGCPrefabRegistry::Register(const FAkUGCPrefabDefinition& Definition, FString* OutError)
{
    if (!ValidateDefinition(Definition, OutError))
    {
        return false;
    }

    if (Definitions.Contains(Definition.PrefabId))
    {
        return Fail(OutError, FString::Printf(TEXT("Prefab '%s' is already registered."), *Definition.PrefabId.ToString()));
    }

    Definitions.Add(Definition.PrefabId, Definition);
    return true;
}

bool FAkUGCPrefabRegistry::Unregister(FName PrefabId)
{
    return Definitions.Remove(PrefabId) > 0;
}

void FAkUGCPrefabRegistry::Reset()
{
    Definitions.Reset();
}

const FAkUGCPrefabDefinition* FAkUGCPrefabRegistry::Find(FName PrefabId) const
{
    return Definitions.Find(PrefabId);
}

const FSoftObjectPath* FAkUGCPrefabRegistry::ResolveAsset(FName PrefabId, FName PlatformVariant) const
{
    const FAkUGCPrefabDefinition* Definition = Find(PrefabId);
    if (!Definition)
    {
        return nullptr;
    }

    if (const FSoftObjectPath* Exact = Definition->AssetVariants.Find(PlatformVariant))
    {
        return Exact;
    }
    return Definition->AssetVariants.Find(TEXT("Default"));
}

TArray<FName> FAkUGCPrefabRegistry::GetRegisteredIds() const
{
    TArray<FName> Result;
    Definitions.GetKeys(Result);
    Result.Sort(FNameLexicalLess());
    return Result;
}

bool FAkUGCPrefabRegistry::ValidateDefinition(const FAkUGCPrefabDefinition& Definition, FString* OutError)
{
    if (Definition.PrefabId.IsNone())
    {
        return Fail(OutError, TEXT("Prefab ID is required."));
    }

    const FString PrefabIdString = Definition.PrefabId.ToString();
    if (!PrefabIdString.Contains(TEXT(".")))
    {
        return Fail(OutError, TEXT("Prefab ID must be namespaced, for example 'official.gameplay.base'."));
    }

    if (Definition.DefinitionVersion < 1)
    {
        return Fail(OutError, TEXT("Prefab definition version must be positive."));
    }

    if (Definition.DisplayName.TrimStartAndEnd().IsEmpty())
    {
        return Fail(OutError, TEXT("Prefab display name is required."));
    }

    if (Definition.EntityType.IsNone())
    {
        return Fail(OutError, TEXT("Prefab entity type is required."));
    }

    if (Definition.SupportedPlatforms == 0)
    {
        return Fail(OutError, TEXT("Prefab must support at least one target platform."));
    }

    if (Definition.Placement.MinimumScale.GetMin() <= 0.0)
    {
        return Fail(OutError, TEXT("Prefab minimum scale must be positive."));
    }

    if ((Definition.Placement.MaximumScale - Definition.Placement.MinimumScale).GetMin() < 0.0)
    {
        return Fail(OutError, TEXT("Prefab maximum scale must be greater than or equal to minimum scale."));
    }

    TSet<FName> PropertyIds;
    for (const FAkUGCPropertyDefinition& Property : Definition.EditableProperties)
    {
        if (Property.PropertyId.IsNone())
        {
            return Fail(OutError, TEXT("Editable property ID is required."));
        }
        if (PropertyIds.Contains(Property.PropertyId))
        {
            return Fail(OutError, FString::Printf(TEXT("Editable property '%s' is duplicated."), *Property.PropertyId.ToString()));
        }
        if (Property.DefaultValue.Type != Property.ValueType)
        {
            return Fail(OutError, FString::Printf(TEXT("Editable property '%s' default value type does not match its schema."), *Property.PropertyId.ToString()));
        }
        if (Property.bHasMinimum && Property.bHasMaximum && Property.Minimum > Property.Maximum)
        {
            return Fail(OutError, FString::Printf(TEXT("Editable property '%s' has an invalid numeric range."), *Property.PropertyId.ToString()));
        }
        PropertyIds.Add(Property.PropertyId);
    }

    return true;
}

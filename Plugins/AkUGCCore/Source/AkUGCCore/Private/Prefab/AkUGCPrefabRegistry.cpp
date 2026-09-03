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

bool FAkUGCPrefabRegistry::RegisterBatch(
    const TArray<FAkUGCPrefabDefinition>& NewDefinitions,
    FString* OutError)
{
    TMap<FName, FAkUGCPrefabDefinition> WorkingDefinitions = Definitions;
    for (const FAkUGCPrefabDefinition& Definition : NewDefinitions)
    {
        if (!ValidateDefinition(Definition, OutError))
        {
            return false;
        }
        if (WorkingDefinitions.Contains(Definition.PrefabId))
        {
            return Fail(OutError, FString::Printf(
                TEXT("Prefab '%s' is already registered."),
                *Definition.PrefabId.ToString()));
        }
        WorkingDefinitions.Add(Definition.PrefabId, Definition);
    }

    Definitions = MoveTemp(WorkingDefinitions);
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

bool FAkUGCPrefabRegistry::CreateEntityRecord(
    FName PrefabId,
    FGuid EntityId,
    const FTransform& Transform,
    FAkUGCEntityRecord& OutEntity,
    FString* OutError) const
{
    const FAkUGCPrefabDefinition* Definition = Find(PrefabId);
    if (!Definition)
    {
        return Fail(OutError, FString::Printf(TEXT("Prefab '%s' is not registered."), *PrefabId.ToString()));
    }
    if (!EntityId.IsValid())
    {
        return Fail(OutError, TEXT("Entity ID must be a valid GUID."));
    }
    if (Transform.ContainsNaN())
    {
        return Fail(OutError, TEXT("Entity transform contains a non-finite value."));
    }

    OutEntity = FAkUGCEntityRecord{};
    OutEntity.EntityId = EntityId;
    OutEntity.PrefabId = PrefabId;
    OutEntity.Transform = Transform;
    OutEntity.Components = Definition->DefaultComponents;

    for (const FAkUGCPropertyDefinition& Property : Definition->EditableProperties)
    {
        FAkUGCComponentRecord* Component = OutEntity.Components.FindByPredicate([&Property](const FAkUGCComponentRecord& Candidate)
        {
            return Candidate.TypeId == Property.ComponentTypeId;
        });
        check(Component);
        Component->Properties.FindOrAdd(Property.PropertyId) = Property.DefaultValue;
    }
    return true;
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

    TSet<FName> ComponentTypeIds;
    for (const FAkUGCComponentRecord& Component : Definition.DefaultComponents)
    {
        if (Component.TypeId.IsNone())
        {
            return Fail(OutError, TEXT("Default component type ID is required."));
        }
        if (ComponentTypeIds.Contains(Component.TypeId))
        {
            return Fail(OutError, FString::Printf(
                TEXT("Default component '%s' is duplicated."),
                *Component.TypeId.ToString()));
        }
        if (Component.SchemaVersion < 1)
        {
            return Fail(OutError, FString::Printf(
                TEXT("Default component '%s' schema version must be positive."),
                *Component.TypeId.ToString()));
        }
        ComponentTypeIds.Add(Component.TypeId);
    }

    TSet<FString> PropertyKeys;
    for (const FAkUGCPropertyDefinition& Property : Definition.EditableProperties)
    {
        if (Property.ComponentTypeId.IsNone())
        {
            return Fail(OutError, TEXT("Editable property component type ID is required."));
        }
        if (!ComponentTypeIds.Contains(Property.ComponentTypeId))
        {
            return Fail(OutError, FString::Printf(
                TEXT("Editable property component '%s' does not exist in default components."),
                *Property.ComponentTypeId.ToString()));
        }
        if (Property.PropertyId.IsNone())
        {
            return Fail(OutError, TEXT("Editable property ID is required."));
        }

        const FString PropertyKey = Property.ComponentTypeId.ToString() + TEXT(".") + Property.PropertyId.ToString();
        if (PropertyKeys.Contains(PropertyKey))
        {
            return Fail(OutError, FString::Printf(TEXT("Editable property '%s' is duplicated."), *PropertyKey));
        }
        if (Property.DefaultValue.Type != Property.ValueType)
        {
            return Fail(OutError, FString::Printf(TEXT("Editable property '%s' default value type does not match its schema."), *PropertyKey));
        }
        if (Property.bHasMinimum && Property.bHasMaximum && Property.Minimum > Property.Maximum)
        {
            return Fail(OutError, FString::Printf(TEXT("Editable property '%s' has an invalid numeric range."), *PropertyKey));
        }
        PropertyKeys.Add(PropertyKey);
    }

    return true;
}

#include "Prefab/AkUGCOfficialPrefabCatalog.h"

#include "Prefab/AkUGCPrefabDefinition.h"
#include "Prefab/AkUGCPrefabRegistry.h"

namespace
{
    constexpr int32 CommonPlatforms = static_cast<int32>(EAkUGCTargetPlatform::Win64)
        | static_cast<int32>(EAkUGCTargetPlatform::Android)
        | static_cast<int32>(EAkUGCTargetPlatform::IOS)
        | static_cast<int32>(EAkUGCTargetPlatform::Server);

    FAkUGCValue NumberValue(double Value)
    {
        FAkUGCValue Result;
        Result.Type = EAkUGCValueType::Number;
        Result.NumberValue = Value;
        return Result;
    }

    FAkUGCValue IntegerValue(int64 Value)
    {
        FAkUGCValue Result;
        Result.Type = EAkUGCValueType::Integer;
        Result.IntegerValue = Value;
        return Result;
    }

    FAkUGCValue NameValue(FName Value)
    {
        FAkUGCValue Result;
        Result.Type = EAkUGCValueType::Name;
        Result.NameValue = Value;
        return Result;
    }

    FAkUGCPrefabDefinition MakePrefab(
        FName PrefabId,
        const TCHAR* DisplayName,
        FName EntityType,
        int32 RenderCost = 1,
        int32 PhysicsCost = 0,
        int32 ScriptCost = 0)
    {
        FAkUGCPrefabDefinition Definition;
        Definition.PrefabId = PrefabId;
        Definition.DisplayName = DisplayName;
        Definition.EntityType = EntityType;
        Definition.SupportedPlatforms = CommonPlatforms;
        Definition.Cost.RenderUnits = RenderCost;
        Definition.Cost.PhysicsUnits = PhysicsCost;
        Definition.Cost.ScriptUnits = ScriptCost;
        return Definition;
    }

    FAkUGCComponentRecord& AddComponent(FAkUGCPrefabDefinition& Definition, FName ComponentTypeId)
    {
        FAkUGCComponentRecord& Component = Definition.DefaultComponents.AddDefaulted_GetRef();
        Component.TypeId = ComponentTypeId;
        return Component;
    }

    void AddProperty(
        FAkUGCPrefabDefinition& Definition,
        FName ComponentTypeId,
        FName PropertyId,
        EAkUGCValueType ValueType,
        const FAkUGCValue& DefaultValue,
        bool bMobileEditable = true,
        TOptional<double> Minimum = {},
        TOptional<double> Maximum = {})
    {
        FAkUGCPropertyDefinition& Property = Definition.EditableProperties.AddDefaulted_GetRef();
        Property.ComponentTypeId = ComponentTypeId;
        Property.PropertyId = PropertyId;
        Property.ValueType = ValueType;
        Property.DefaultValue = DefaultValue;
        Property.bMobileEditable = bMobileEditable;
        if (Minimum.IsSet())
        {
            Property.bHasMinimum = true;
            Property.Minimum = Minimum.GetValue();
        }
        if (Maximum.IsSet())
        {
            Property.bHasMaximum = true;
            Property.Maximum = Maximum.GetValue();
        }
    }
}

TArray<FAkUGCPrefabDefinition> FAkUGCOfficialPrefabCatalog::BuildTowerDefenseDefinitions()
{
    TArray<FAkUGCPrefabDefinition> Definitions;
    Definitions.Reserve(11);

    {
        FAkUGCPrefabDefinition Definition = MakePrefab(
            TEXT("official.environment.ground"), TEXT("Ground"), TEXT("Environment.Ground"), 1);
        Definition.Placement.bAllowScale = true;
        Definition.Placement.MinimumScale = FVector(0.25);
        Definition.Placement.MaximumScale = FVector(20.0);
        AddComponent(Definition, TEXT("core.visual"));
        Definitions.Add(MoveTemp(Definition));
    }
    {
        FAkUGCPrefabDefinition Definition = MakePrefab(
            TEXT("official.environment.wall"), TEXT("Wall"), TEXT("Environment.Wall"), 1, 1);
        Definition.Placement.bAllowScale = true;
        Definition.Placement.MinimumScale = FVector(0.25);
        Definition.Placement.MaximumScale = FVector(10.0);
        AddComponent(Definition, TEXT("core.visual"));
        AddComponent(Definition, TEXT("core.collision"));
        Definitions.Add(MoveTemp(Definition));
    }
    {
        FAkUGCPrefabDefinition Definition = MakePrefab(
            TEXT("official.environment.tree"), TEXT("Tree"), TEXT("Environment.Decoration"), 1, 1);
        Definition.Placement.bAllowScale = true;
        Definition.Placement.MinimumScale = FVector(0.5);
        Definition.Placement.MaximumScale = FVector(2.0);
        AddComponent(Definition, TEXT("core.visual"));
        AddComponent(Definition, TEXT("core.collision"));
        Definitions.Add(MoveTemp(Definition));
    }
    {
        FAkUGCPrefabDefinition Definition = MakePrefab(
            TEXT("official.gameplay.base"), TEXT("Base"), TEXT("Gameplay.Base"), 2, 2, 1);
        AddComponent(Definition, TEXT("core.health"));
        AddComponent(Definition, TEXT("core.team"));
        AddProperty(Definition, TEXT("core.health"), TEXT("maxHealth"), EAkUGCValueType::Number, NumberValue(1000.0), true, 1.0, 100000.0);
        AddProperty(Definition, TEXT("core.team"), TEXT("teamId"), EAkUGCValueType::Integer, IntegerValue(1), true, 0.0, 16.0);
        Definitions.Add(MoveTemp(Definition));
    }
    {
        FAkUGCPrefabDefinition Definition = MakePrefab(
            TEXT("official.gameplay.enemy_spawn"), TEXT("Enemy Spawn"), TEXT("Gameplay.EnemySpawn"), 0, 0, 2);
        AddComponent(Definition, TEXT("tower_defense.spawn"));
        AddProperty(Definition, TEXT("tower_defense.spawn"), TEXT("enemyPrefab"), EAkUGCValueType::Name, NameValue(TEXT("official.unit.basic_enemy")));
        AddProperty(Definition, TEXT("tower_defense.spawn"), TEXT("spawnInterval"), EAkUGCValueType::Number, NumberValue(1.0), true, 0.1, 60.0);
        AddProperty(Definition, TEXT("tower_defense.spawn"), TEXT("enemyCount"), EAkUGCValueType::Integer, IntegerValue(10), true, 1.0, 500.0);
        Definitions.Add(MoveTemp(Definition));
    }
    {
        FAkUGCPrefabDefinition Definition = MakePrefab(
            TEXT("official.gameplay.path_node"), TEXT("Path Node"), TEXT("Gameplay.PathNode"), 0, 0, 1);
        AddComponent(Definition, TEXT("tower_defense.path_node"));
        AddProperty(Definition, TEXT("tower_defense.path_node"), TEXT("order"), EAkUGCValueType::Integer, IntegerValue(0), true, 0.0, 10000.0);
        Definitions.Add(MoveTemp(Definition));
    }
    {
        FAkUGCPrefabDefinition Definition = MakePrefab(
            TEXT("official.gameplay.goal"), TEXT("Goal"), TEXT("Gameplay.Goal"), 1, 1, 1);
        AddComponent(Definition, TEXT("tower_defense.goal"));
        AddProperty(Definition, TEXT("tower_defense.goal"), TEXT("baseDamage"), EAkUGCValueType::Number, NumberValue(10.0), true, 0.0, 100000.0);
        Definitions.Add(MoveTemp(Definition));
    }
    {
        FAkUGCPrefabDefinition Definition = MakePrefab(
            TEXT("official.gameplay.tower_slot"), TEXT("Tower Slot"), TEXT("Gameplay.TowerSlot"), 1, 1, 1);
        AddComponent(Definition, TEXT("tower_defense.tower_slot"));
        AddProperty(Definition, TEXT("tower_defense.tower_slot"), TEXT("allowedTowerTag"), EAkUGCValueType::Name, NameValue(TEXT("tower.basic")));
        Definitions.Add(MoveTemp(Definition));
    }
    {
        FAkUGCPrefabDefinition Definition = MakePrefab(
            TEXT("official.gameplay.trigger_volume"), TEXT("Trigger Volume"), TEXT("Gameplay.TriggerVolume"), 0, 1, 1);
        AddComponent(Definition, TEXT("core.trigger_volume"));
        AddProperty(Definition, TEXT("core.trigger_volume"), TEXT("radius"), EAkUGCValueType::Number, NumberValue(300.0), true, 10.0, 10000.0);
        Definitions.Add(MoveTemp(Definition));
    }
    {
        FAkUGCPrefabDefinition Definition = MakePrefab(
            TEXT("official.unit.basic_enemy"), TEXT("Basic Enemy"), TEXT("Unit.Enemy"), 2, 2, 2);
        AddComponent(Definition, TEXT("core.health"));
        AddComponent(Definition, TEXT("core.team"));
        AddComponent(Definition, TEXT("tower_defense.enemy"));
        AddProperty(Definition, TEXT("core.health"), TEXT("maxHealth"), EAkUGCValueType::Number, NumberValue(100.0), true, 1.0, 100000.0);
        AddProperty(Definition, TEXT("core.team"), TEXT("teamId"), EAkUGCValueType::Integer, IntegerValue(2), false, 0.0, 16.0);
        AddProperty(Definition, TEXT("tower_defense.enemy"), TEXT("moveSpeed"), EAkUGCValueType::Number, NumberValue(300.0), true, 10.0, 2000.0);
        AddProperty(Definition, TEXT("tower_defense.enemy"), TEXT("goalDamage"), EAkUGCValueType::Number, NumberValue(10.0), true, 0.0, 100000.0);
        Definitions.Add(MoveTemp(Definition));
    }
    {
        FAkUGCPrefabDefinition Definition = MakePrefab(
            TEXT("official.tower.basic"), TEXT("Basic Tower"), TEXT("Tower.Basic"), 3, 1, 2);
        AddComponent(Definition, TEXT("core.team"));
        AddComponent(Definition, TEXT("tower_defense.tower"));
        AddProperty(Definition, TEXT("core.team"), TEXT("teamId"), EAkUGCValueType::Integer, IntegerValue(1), false, 0.0, 16.0);
        AddProperty(Definition, TEXT("tower_defense.tower"), TEXT("attackDamage"), EAkUGCValueType::Number, NumberValue(25.0), true, 0.0, 100000.0);
        AddProperty(Definition, TEXT("tower_defense.tower"), TEXT("attackRange"), EAkUGCValueType::Number, NumberValue(800.0), true, 10.0, 10000.0);
        AddProperty(Definition, TEXT("tower_defense.tower"), TEXT("attackInterval"), EAkUGCValueType::Number, NumberValue(1.0), true, 0.05, 60.0);
        Definitions.Add(MoveTemp(Definition));
    }

    return Definitions;
}

TArray<FName> FAkUGCOfficialPrefabCatalog::GetTowerDefensePrefabIds()
{
    TArray<FName> Result;
    for (const FAkUGCPrefabDefinition& Definition : BuildTowerDefenseDefinitions())
    {
        Result.Add(Definition.PrefabId);
    }
    return Result;
}

bool FAkUGCOfficialPrefabCatalog::RegisterTowerDefense(
    FAkUGCPrefabRegistry& Registry,
    FString* OutError)
{
    return Registry.RegisterBatch(BuildTowerDefenseDefinitions(), OutError);
}

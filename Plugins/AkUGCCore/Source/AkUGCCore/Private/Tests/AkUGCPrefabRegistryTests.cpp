#include "Misc/AutomationTest.h"

#include "Prefab/AkUGCPrefabRegistry.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
    FAkUGCPrefabDefinition MakeBasePrefab()
    {
        FAkUGCPrefabDefinition Definition;
        Definition.PrefabId = TEXT("official.gameplay.base");
        Definition.DisplayName = TEXT("Base");
        Definition.EntityType = TEXT("Gameplay.Base");
        Definition.AssetVariants.Add(TEXT("Win64"), FSoftObjectPath(TEXT("/Game/UGC/Prefabs/Base_Win64.Base_Win64")));
        Definition.AssetVariants.Add(TEXT("Default"), FSoftObjectPath(TEXT("/Game/UGC/Prefabs/Base_Mobile.Base_Mobile")));

        FAkUGCComponentRecord& HealthComponent = Definition.DefaultComponents.AddDefaulted_GetRef();
        HealthComponent.TypeId = TEXT("core.health");

        FAkUGCPropertyDefinition Health;
        Health.ComponentTypeId = TEXT("core.health");
        Health.PropertyId = TEXT("maxHealth");
        Health.ValueType = EAkUGCValueType::Number;
        Health.DefaultValue.Type = EAkUGCValueType::Number;
        Health.DefaultValue.NumberValue = 1000.0;
        Health.bHasMinimum = true;
        Health.Minimum = 1.0;
        Health.bHasMaximum = true;
        Health.Maximum = 100000.0;
        Definition.EditableProperties.Add(Health);
        return Definition;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCPrefabRegistryTest,
    "AkUGC.Core.Prefab.Registry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCPrefabRegistryTest::RunTest(const FString& Parameters)
{
    FAkUGCPrefabRegistry Registry;
    const FAkUGCPrefabDefinition Definition = MakeBasePrefab();
    FString Error;

    TestTrue(TEXT("Valid prefab registers"), Registry.Register(Definition, &Error));
    TestNotNull(TEXT("Registered prefab can be found"), Registry.Find(Definition.PrefabId));
    TestFalse(TEXT("Duplicate prefab is rejected"), Registry.Register(Definition, &Error));

    const FSoftObjectPath* Win64Asset = Registry.ResolveAsset(Definition.PrefabId, TEXT("Win64"));
    TestNotNull(TEXT("Exact platform asset resolves"), Win64Asset);
    if (Win64Asset)
    {
        TestEqual(TEXT("Exact platform asset is selected"), Win64Asset->ToString(), FString(TEXT("/Game/UGC/Prefabs/Base_Win64.Base_Win64")));
    }

    const FSoftObjectPath* FallbackAsset = Registry.ResolveAsset(Definition.PrefabId, TEXT("Android"));
    TestNotNull(TEXT("Default platform asset resolves"), FallbackAsset);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCPrefabValidationTest,
    "AkUGC.Core.Prefab.Validation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCPrefabValidationTest::RunTest(const FString& Parameters)
{
    FAkUGCPrefabDefinition Definition = MakeBasePrefab();
    Definition.PrefabId = TEXT("not_namespaced");

    FString Error;
    TestFalse(TEXT("Non-namespaced prefab ID is rejected"), FAkUGCPrefabRegistry::ValidateDefinition(Definition, &Error));
    TestFalse(TEXT("Validation returns an error message"), Error.IsEmpty());

    Definition = MakeBasePrefab();
    Definition.EditableProperties[0].Maximum = TNumericLimits<double>::Max() * 2.0;
    TestFalse(TEXT("Non-finite property range is rejected"), FAkUGCPrefabRegistry::ValidateDefinition(Definition, &Error));

    Definition = MakeBasePrefab();
    Definition.EditableProperties[0].DefaultValue.NumberValue = 200000.0;
    TestFalse(TEXT("Out-of-range property default is rejected"), FAkUGCPrefabRegistry::ValidateDefinition(Definition, &Error));
    return true;
}

#endif

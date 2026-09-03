#include "Misc/AutomationTest.h"

#include "Prefab/AkUGCOfficialPrefabCatalog.h"
#include "Prefab/AkUGCPrefabRegistry.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCOfficialPrefabCatalogTest,
    "AkUGC.Core.Prefab.OfficialTowerDefenseCatalog",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCOfficialPrefabCatalogTest::RunTest(const FString& Parameters)
{
    const TArray<FAkUGCPrefabDefinition> Definitions = FAkUGCOfficialPrefabCatalog::BuildTowerDefenseDefinitions();
    TestEqual(TEXT("Tower defense catalog contains eleven prefabs"), Definitions.Num(), 11);

    FString Error;
    for (const FAkUGCPrefabDefinition& Definition : Definitions)
    {
        TestTrue(
            *FString::Printf(TEXT("Prefab '%s' is valid"), *Definition.PrefabId.ToString()),
            FAkUGCPrefabRegistry::ValidateDefinition(Definition, &Error));
    }

    FAkUGCPrefabRegistry Registry;
    TestTrue(TEXT("Official catalog registers atomically"), FAkUGCOfficialPrefabCatalog::RegisterTowerDefense(Registry, &Error));
    TestEqual(TEXT("Registry contains all official prefabs"), Registry.GetRegisteredIds().Num(), 11);
    TestNotNull(TEXT("Base prefab is registered"), Registry.Find(TEXT("official.gameplay.base")));
    TestNotNull(TEXT("Enemy prefab is registered"), Registry.Find(TEXT("official.unit.basic_enemy")));
    TestNotNull(TEXT("Tower prefab is registered"), Registry.Find(TEXT("official.tower.basic")));

    FAkUGCEntityRecord BaseEntity;
    const FGuid EntityId = FGuid::NewGuid();
    TestTrue(
        TEXT("Base entity is instantiated from prefab defaults"),
        Registry.CreateEntityRecord(
            TEXT("official.gameplay.base"),
            EntityId,
            FTransform(FVector(100.0, 200.0, 0.0)),
            BaseEntity,
            &Error));
    TestEqual(TEXT("Instantiated entity keeps requested ID"), BaseEntity.EntityId, EntityId);
    TestEqual(TEXT("Base prefab creates two components"), BaseEntity.Components.Num(), 2);

    const FAkUGCComponentRecord* Health = BaseEntity.Components.FindByPredicate([](const FAkUGCComponentRecord& Component)
    {
        return Component.TypeId == TEXT("core.health");
    });
    TestNotNull(TEXT("Base entity contains health component"), Health);
    if (Health)
    {
        const FAkUGCValue* MaxHealth = Health->Properties.Find(TEXT("maxHealth"));
        TestNotNull(TEXT("Base entity contains maxHealth default"), MaxHealth);
        if (MaxHealth)
        {
            TestEqual(TEXT("Base maxHealth default is 1000"), MaxHealth->NumberValue, 1000.0);
        }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCPrefabBatchAtomicityTest,
    "AkUGC.Core.Prefab.BatchRegistrationIsAtomic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCPrefabBatchAtomicityTest::RunTest(const FString& Parameters)
{
    FAkUGCPrefabRegistry Registry;
    FString Error;

    FAkUGCPrefabDefinition Valid;
    Valid.PrefabId = TEXT("creator.valid.prefab");
    Valid.DisplayName = TEXT("Valid");
    Valid.EntityType = TEXT("Test.Entity");

    FAkUGCPrefabDefinition Invalid = Valid;
    Invalid.PrefabId = TEXT("invalid");

    TestFalse(TEXT("Invalid batch is rejected"), Registry.RegisterBatch({Valid, Invalid}, &Error));
    TestEqual(TEXT("Rejected batch leaves registry unchanged"), Registry.GetRegisteredIds().Num(), 0);
    return true;
}

#endif

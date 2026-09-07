#include "Misc/AutomationTest.h"

#include "Command/AkUGCRuntimeCommandService.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Prefab/AkUGCOfficialPrefabCatalog.h"
#include "Prefab/AkUGCPrefabRegistry.h"
#include "Scene/AkUGCSceneRuntime.h"
#include "Session/AkUGCDocumentRuntimeSession.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
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

    FAkUGCPrefabDefinition MakeEditablePrefab()
    {
        FAkUGCPrefabDefinition Prefab;
        Prefab.PrefabId = TEXT("official.test.editable");
        Prefab.DisplayName = TEXT("Editable Test Prefab");
        Prefab.EntityType = TEXT("Test.Editable");

        FAkUGCComponentRecord& Component = Prefab.DefaultComponents.AddDefaulted_GetRef();
        Component.TypeId = TEXT("test.stats");

        FAkUGCPropertyDefinition& Property = Prefab.EditableProperties.AddDefaulted_GetRef();
        Property.ComponentTypeId = Component.TypeId;
        Property.PropertyId = TEXT("power");
        Property.ValueType = EAkUGCValueType::Number;
        Property.DefaultValue = NumberValue(10.0);
        Property.bHasMinimum = true;
        Property.Minimum = 1.0;
        Property.bHasMaximum = true;
        Property.Maximum = 100.0;

        FAkUGCPropertyDefinition& DesktopProperty = Prefab.EditableProperties.AddDefaulted_GetRef();
        DesktopProperty.ComponentTypeId = Component.TypeId;
        DesktopProperty.PropertyId = TEXT("desktopOnly");
        DesktopProperty.ValueType = EAkUGCValueType::Number;
        DesktopProperty.DefaultValue = NumberValue(5.0);
        DesktopProperty.bMobileEditable = false;

        FAkUGCPropertyDefinition& LargeIntegerProperty = Prefab.EditableProperties.AddDefaulted_GetRef();
        LargeIntegerProperty.ComponentTypeId = Component.TypeId;
        LargeIntegerProperty.PropertyId = TEXT("largeInteger");
        LargeIntegerProperty.ValueType = EAkUGCValueType::Integer;
        LargeIntegerProperty.DefaultValue = IntegerValue(0);
        LargeIntegerProperty.bHasMaximum = true;
        LargeIntegerProperty.Maximum = 9007199254740992.0;
        return Prefab;
    }

    FAkUGCProjectDocument MakeDocument(FGuid& OutSceneId)
    {
        FAkUGCProjectDocument Document;
        Document.Manifest.ProjectId = FGuid::NewGuid();
        Document.Manifest.DisplayName = TEXT("Runtime Command Service Test");
        Document.Manifest.TemplateId = TEXT("official.test");
        FAkUGCSceneDocument& Scene = Document.Scenes.AddDefaulted_GetRef();
        Scene.SceneId = FGuid::NewGuid();
        Scene.DisplayName = TEXT("Main");
        OutSceneId = Scene.SceneId;
        return Document;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCRuntimeCommandServiceWorkflowTest,
    "AkUGC.Runtime.CommandService.SharedEditingWorkflow",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCRuntimeCommandServiceWorkflowTest::RunTest(const FString& Parameters)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AkUGCRuntimeCommandServiceTest"));
    if (!World)
    {
        AddError(TEXT("Failed to create test world."));
        return false;
    }
    if (!GEngine)
    {
        AddError(TEXT("Engine is not available."));
        World->DestroyWorld(false);
        return false;
    }
    FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
    WorldContext.SetCurrentWorld(World);

    FGuid SceneId;
    FAkUGCProjectDocument Document = MakeDocument(SceneId);
    FAkUGCPrefabRegistry Registry;
    FString RegistryError;
    if (!Registry.Register(MakeEditablePrefab(), &RegistryError))
    {
        AddError(FString::Printf(TEXT("Editable prefab registration failed: %s"), *RegistryError));
        GEngine->DestroyWorldContext(World);
        World->DestroyWorld(false);
        return false;
    }

    {
        FAkUGCSceneRuntime Runtime(World);
        FAkUGCDocumentRuntimeSession Session(Runtime, Registry, SceneId);
        const FAkUGCCommandExecutionResult InitializeResult = Session.Initialize(Document);
        if (!InitializeResult.bSucceeded)
        {
            AddError(InitializeResult.ErrorMessage);
            GEngine->DestroyWorldContext(World);
            World->DestroyWorld(false);
            return false;
        }
        FAkUGCRuntimeCommandService Service(Document, Session, Registry, SceneId);

        FGuid ParentId;
        FGuid ChildId;
        if (!Service.PlacePrefab(
            TEXT("official.test.editable"), FTransform(FVector(100.0, 0.0, 0.0)), ParentId).bSucceeded
            || !Service.PlacePrefab(
                TEXT("official.test.editable"), FTransform(FVector(250.0, 0.0, 0.0)), ChildId).bSucceeded)
        {
            AddError(TEXT("Shared service failed to place required entities."));
            Runtime.Unload();
            GEngine->DestroyWorldContext(World);
            World->DestroyWorld(false);
            return false;
        }
        TestTrue(TEXT("Shared service parents child"), Service.SetEntityParent(ChildId, ParentId).bSucceeded);
        AActor* ParentActor = Runtime.FindActor(ParentId);
        AActor* ChildActor = Runtime.FindActor(ChildId);
        TestNotNull(TEXT("Runtime parent exists"), ParentActor);
        TestNotNull(TEXT("Runtime child exists"), ChildActor);
        if (!ParentActor || !ChildActor)
        {
            Runtime.Unload();
            GEngine->DestroyWorldContext(World);
            World->DestroyWorld(false);
            return false;
        }
        TestEqual(TEXT("Runtime child is attached"), ChildActor->GetAttachParentActor(), ParentActor);

        FAkUGCValue Power = NumberValue(75.0);
        TestTrue(TEXT("Shared service edits exposed property"), Service.SetEntityProperty(
            ParentId, TEXT("test.stats"), TEXT("power"), Power).bSucceeded);
        Power.NumberValue = 150.0;
        TestFalse(TEXT("Shared service rejects out-of-range property"), Service.SetEntityProperty(
            ParentId, TEXT("test.stats"), TEXT("power"), Power).bSucceeded);

        FAkUGCRuntimeCommandService MobileService(
            Document,
            Session,
            Registry,
            SceneId,
            EAkUGCEditingClient::Mobile);
        TestFalse(TEXT("Mobile service rejects desktop-only property"), MobileService.SetEntityProperty(
            ParentId, TEXT("test.stats"), TEXT("desktopOnly"), NumberValue(6.0)).bSucceeded);
        TestFalse(TEXT("Integer range rejects value above exact 2^53 boundary"), Service.SetEntityProperty(
            ParentId,
            TEXT("test.stats"),
            TEXT("largeInteger"),
            IntegerValue(9007199254740993LL)).bSucceeded);

        ParentActor->SetActorLocation(FVector(999.0, 0.0, 0.0));
        FGuid DuplicateId;
        TestTrue(TEXT("Shared service duplicates from document truth"), Service.DuplicateEntity(
            ParentId, FVector(10.0, 20.0, 0.0), DuplicateId).bSucceeded);
        const FAkUGCEntityRecord* Duplicate = Service.FindEntity(DuplicateId);
        TestNotNull(TEXT("Duplicate exists in document"), Duplicate);
        if (!Duplicate)
        {
            Runtime.Unload();
            GEngine->DestroyWorldContext(World);
            World->DestroyWorld(false);
            return false;
        }
        TestEqual(
            TEXT("Duplicate transform comes from document instead of preview actor"),
            Duplicate->Transform.GetLocation(),
            FVector(110.0, 20.0, 0.0));

        TestTrue(TEXT("Hierarchy-safe delete succeeds"), Service.DeleteEntities({ParentId}, TEXT("Delete parent")).bSucceeded);
        TestNull(TEXT("Deleted parent leaves runtime"), Runtime.FindActor(ParentId));
        const FAkUGCEntityRecord* SurvivingChild = Service.FindEntity(ChildId);
        TestNotNull(TEXT("Child survives parent deletion"), SurvivingChild);
        if (SurvivingChild)
        {
            TestFalse(TEXT("Surviving child moves to scene root"), SurvivingChild->ParentEntityId.IsValid());
        }
        TestNull(TEXT("Runtime child moves to scene root"), Runtime.FindActor(ChildId)->GetAttachParentActor());

        TestTrue(TEXT("Shared service undo restores deletion"), Service.Undo().bSucceeded);
        TestNotNull(TEXT("Undo restores parent runtime actor"), Runtime.FindActor(ParentId));
        TestEqual(TEXT("Undo restores child attachment"), Runtime.FindActor(ChildId)->GetAttachParentActor(), Runtime.FindActor(ParentId));
        TestTrue(TEXT("Shared service redo reapplies deletion"), Service.Redo().bSucceeded);
        TestNull(TEXT("Redo removes parent runtime actor"), Runtime.FindActor(ParentId));
        Runtime.Unload();
    }

    GEngine->DestroyWorldContext(World);
    World->DestroyWorld(false);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCRulesetCommandServiceWorkflowTest,
    "AkUGC.Runtime.CommandService.RulesetWorkflow",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCRulesetCommandServiceWorkflowTest::RunTest(const FString& Parameters)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AkUGCRulesetCommandServiceTest"));
    if (!World || !GEngine)
    {
        AddError(TEXT("Ruleset test world is not available."));
        if (World)
        {
            World->DestroyWorld(false);
        }
        return false;
    }
    FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
    WorldContext.SetCurrentWorld(World);

    FGuid SceneId;
    FAkUGCProjectDocument Document = MakeDocument(SceneId);
    FAkUGCPrefabRegistry Registry;
    FString Error;
    TestTrue(TEXT("Official tower defense prefabs register"),
        FAkUGCOfficialPrefabCatalog::RegisterTowerDefense(Registry, &Error));

    {
        FAkUGCSceneRuntime Runtime(World);
        FAkUGCDocumentRuntimeSession Session(Runtime, Registry, SceneId);
        TestTrue(TEXT("Ruleset edit session initializes"), Session.Initialize(Document).bSucceeded);
        FAkUGCRuntimeCommandService Service(Document, Session, Registry, SceneId);

        TArray<FGuid> SpawnPointIds;
        TArray<FGuid> WaveIds;
        for (int32 WaveIndex = 0; WaveIndex < AkUGCTowerDefenseRulesetLimits::RequiredWaveCount; ++WaveIndex)
        {
            FGuid SpawnPointId;
            TestTrue(TEXT("Enemy Spawn placement succeeds"), Service.PlacePrefab(
                TEXT("official.gameplay.enemy_spawn"),
                FTransform(FVector(WaveIndex * 100.0, 0.0, 0.0)),
                SpawnPointId).bSucceeded);
            SpawnPointIds.Add(SpawnPointId);

            FAkUGCTowerDefenseWave Wave;
            Wave.WaveId = FGuid::NewGuid();
            Wave.SpawnPointEntityId = SpawnPointId;
            Wave.StartDelaySeconds = WaveIndex;
            WaveIds.Add(Wave.WaveId);
            TestTrue(TEXT("Wave add command succeeds"), Service.AddWave(Wave).bSucceeded);
        }
        TestEqual(TEXT("Ruleset contains three waves"), Document.Scenes[0].Ruleset.Waves.Num(), 3);

        FAkUGCTowerDefenseWave FourthWave = Document.Scenes[0].Ruleset.Waves[0];
        FourthWave.WaveId = FGuid::NewGuid();
        TestFalse(TEXT("Fourth wave is rejected atomically"), Service.AddWave(FourthWave).bSucceeded);
        TestEqual(TEXT("Rejected fourth wave leaves Ruleset unchanged"), Document.Scenes[0].Ruleset.Waves.Num(), 3);

        TestTrue(TEXT("Wave move succeeds"), Service.MoveWave(WaveIds[2], 0).bSucceeded);
        TestEqual(TEXT("Wave move changes order"), Document.Scenes[0].Ruleset.Waves[0].WaveId, WaveIds[2]);
        TestTrue(TEXT("Undo wave move succeeds"), Service.Undo().bSucceeded);
        TestEqual(TEXT("Undo restores wave order"), Document.Scenes[0].Ruleset.Waves[0].WaveId, WaveIds[0]);
        TestTrue(TEXT("Redo wave move succeeds"), Service.Redo().bSucceeded);
        TestEqual(TEXT("Redo restores moved wave"), Document.Scenes[0].Ruleset.Waves[0].WaveId, WaveIds[2]);

        FAkUGCTowerDefenseWave UpdatedWave = Document.Scenes[0].Ruleset.Waves[0];
        UpdatedWave.StartDelaySeconds = 12.0;
        TestTrue(TEXT("Wave update succeeds"), Service.UpdateWave(UpdatedWave).bSucceeded);
        TestEqual(TEXT("Wave update changes delay"), Document.Scenes[0].Ruleset.Waves[0].StartDelaySeconds, 12.0);
        TestTrue(TEXT("Undo wave update succeeds"), Service.Undo().bSucceeded);
        TestEqual(TEXT("Undo restores wave delay"), Document.Scenes[0].Ruleset.Waves[0].StartDelaySeconds, 2.0);

        TestTrue(TEXT("Ruleset settings update succeeds"), Service.SetRulesetSettings(
            8.0,
            EAkUGCTowerDefenseDefeatCondition::BaseHealthDepleted,
            EAkUGCTowerDefenseVictoryCondition::AllWavesCleared).bSucceeded);
        TestEqual(TEXT("Ruleset settings update interval"), Document.Scenes[0].Ruleset.WaveIntervalSeconds, 8.0);
        TestTrue(TEXT("Undo Ruleset settings succeeds"), Service.Undo().bSucceeded);
        TestEqual(TEXT("Undo restores Ruleset interval"), Document.Scenes[0].Ruleset.WaveIntervalSeconds, 5.0);

        TestFalse(TEXT("Referenced Enemy Spawn cannot be deleted"),
            Service.DeleteEntities({SpawnPointIds[0]}, TEXT("Reject referenced Spawn Point")).bSucceeded);
        TestNotNull(TEXT("Rejected deletion preserves runtime Actor"), Runtime.FindActor(SpawnPointIds[0]));

        TestTrue(TEXT("Referenced wave deletion succeeds"), Service.DeleteWave(WaveIds[0]).bSucceeded);
        TestTrue(TEXT("Enemy Spawn deletion succeeds after wave deletion"),
            Service.DeleteEntities({SpawnPointIds[0]}, TEXT("Delete unreferenced Spawn Point")).bSucceeded);
        TestNull(TEXT("Deleted unreferenced Spawn Point leaves runtime"), Runtime.FindActor(SpawnPointIds[0]));
        TestTrue(TEXT("Undo restores Enemy Spawn"), Service.Undo().bSucceeded);
        TestNotNull(TEXT("Undo restores Enemy Spawn runtime Actor"), Runtime.FindActor(SpawnPointIds[0]));
        TestTrue(TEXT("Undo restores deleted wave at original position"), Service.Undo().bSucceeded);
        TestEqual(TEXT("Restored wave returns to original position"), Document.Scenes[0].Ruleset.Waves[1].WaveId, WaveIds[0]);
    }

    GEngine->DestroyWorldContext(World);
    World->DestroyWorld(false);
    return true;
}

#endif

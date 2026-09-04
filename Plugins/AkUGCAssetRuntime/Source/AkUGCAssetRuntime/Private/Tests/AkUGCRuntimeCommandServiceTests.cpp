#include "Misc/AutomationTest.h"

#include "Command/AkUGCRuntimeCommandService.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
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

#endif

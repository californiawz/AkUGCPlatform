#include "Misc/AutomationTest.h"

#include "Algo/Reverse.h"
#include "Command/AkUGCCommand.h"
#include "Command/AkUGCRuntimeCommandService.h"
#include "Document/AkUGCDocument.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Gameplay/AkUGCTowerDefensePath.h"
#include "Prefab/AkUGCOfficialPrefabCatalog.h"
#include "Prefab/AkUGCPrefabRegistry.h"
#include "Scene/AkUGCSceneRuntime.h"
#include "Session/AkUGCDocumentRuntimeSession.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
    FAkUGCEntityRecord MakePathNode(
        const FGuid& EntityId,
        int64 Order,
        const FVector& Location)
    {
        FAkUGCEntityRecord Entity;
        Entity.EntityId = EntityId;
        Entity.PrefabId = TEXT("official.gameplay.path_node");
        Entity.Transform.SetLocation(Location);
        FAkUGCComponentRecord& Component = Entity.Components.AddDefaulted_GetRef();
        Component.TypeId = TEXT("tower_defense.path_node");
        FAkUGCValue OrderValue;
        OrderValue.Type = EAkUGCValueType::Integer;
        OrderValue.IntegerValue = Order;
        Component.Properties.Add(TEXT("order"), OrderValue);
        return Entity;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCTowerDefensePathBuilderTest,
    "AkUGC.Runtime.Path.BuildsDeterministically",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCTowerDefensePathBuilderTest::RunTest(const FString& Parameters)
{
    FAkUGCSceneDocument Scene;
    Scene.SceneId = FGuid::NewGuid();
    const FGuid FirstId(3, 0, 0, 0);
    const FGuid SecondId(2, 0, 0, 0);
    const FGuid ThirdId(1, 0, 0, 0);
    Scene.Entities = {
        MakePathNode(ThirdId, 30, FVector(300.0, 0.0, 0.0)),
        MakePathNode(FirstId, 10, FVector(100.0, 0.0, 0.0)),
        MakePathNode(SecondId, 20, FVector(200.0, 0.0, 0.0))};

    const FAkUGCTowerDefensePathBuildResult Result = FAkUGCTowerDefensePathBuilder::Build(Scene, true);
    TestTrue(TEXT("Complete path builds"), Result.bSucceeded);
    TestTrue(TEXT("Path presence is reported"), Result.bPathPresent);
    TestEqual(TEXT("Three path nodes are built"), Result.Path.Num(), 3);
    if (Result.Path.Num() == 3)
    {
        TestEqual(TEXT("First node is ordered by order value"), Result.Path.Nodes[0].EntityId, FirstId);
        TestEqual(TEXT("Second node is ordered by order value"), Result.Path.Nodes[1].EntityId, SecondId);
        TestEqual(TEXT("Third node is ordered by order value"), Result.Path.Nodes[2].EntityId, ThirdId);
        TestEqual(TEXT("Path location comes from Document world transform"), Result.Path.Nodes[1].Location, FVector(200.0, 0.0, 0.0));
    }

    Algo::Reverse(Scene.Entities);
    const FAkUGCTowerDefensePathBuildResult ReorderedResult = FAkUGCTowerDefensePathBuilder::Build(Scene, true);
    TestTrue(TEXT("Reordered source path builds"), ReorderedResult.bSucceeded);
    if (ReorderedResult.Path.Num() == 3)
    {
        TestEqual(TEXT("Source array order does not affect first path node"), ReorderedResult.Path.Nodes[0].EntityId, FirstId);
        TestEqual(TEXT("Source array order does not affect last path node"), ReorderedResult.Path.Nodes[2].EntityId, ThirdId);
    }

    FAkUGCSceneDocument EmptyScene;
    EmptyScene.SceneId = FGuid::NewGuid();
    TestTrue(TEXT("Empty path is allowed while editing"), FAkUGCTowerDefensePathBuilder::Build(EmptyScene).bSucceeded);
    TestFalse(TEXT("Empty path is rejected when gameplay requires a usable path"),
        FAkUGCTowerDefensePathBuilder::Build(EmptyScene, true).bSucceeded);

    FAkUGCSceneDocument PartialScene;
    PartialScene.SceneId = FGuid::NewGuid();
    PartialScene.Entities.Add(MakePathNode(FGuid::NewGuid(), 0, FVector::ZeroVector));
    TestTrue(TEXT("One path node is allowed as an editing intermediate"),
        FAkUGCTowerDefensePathBuilder::Build(PartialScene).bSucceeded);
    TestFalse(TEXT("One path node is not a usable gameplay path"),
        FAkUGCTowerDefensePathBuilder::Build(PartialScene, true).bSucceeded);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCTowerDefensePathValidationTest,
    "AkUGC.Runtime.Path.RejectsInvalidData",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCTowerDefensePathValidationTest::RunTest(const FString& Parameters)
{
    FAkUGCSceneDocument Scene;
    Scene.SceneId = FGuid::NewGuid();
    Scene.Entities = {
        MakePathNode(FGuid(1, 0, 0, 0), 10, FVector::ZeroVector),
        MakePathNode(FGuid(2, 0, 0, 0), 10, FVector(100.0, 0.0, 0.0))};
    TestFalse(TEXT("Duplicate path order is rejected"), FAkUGCTowerDefensePathBuilder::Build(Scene).bSucceeded);

    Scene.Entities[1].Components[0].Properties.FindChecked(TEXT("order")).IntegerValue = 20;
    Scene.Entities[1].Transform.SetLocation(FVector::ZeroVector);
    TestFalse(TEXT("Consecutive zero-length path segment is rejected"), FAkUGCTowerDefensePathBuilder::Build(Scene).bSucceeded);

    Scene.Entities[1].Transform.SetLocation(FVector(100.0, 0.0, 0.0));
    Scene.Entities[1].Components[0].Properties.FindChecked(TEXT("order")).Type = EAkUGCValueType::Number;
    TestFalse(TEXT("Number order is not coerced to Integer"), FAkUGCTowerDefensePathBuilder::Build(Scene).bSucceeded);

    Scene.Entities[1].Components[0].Properties.FindChecked(TEXT("order")).Type = EAkUGCValueType::Integer;
    Scene.Entities[1].Components[0].Properties.FindChecked(TEXT("order")).IntegerValue = 10001;
    TestFalse(TEXT("Out-of-range path order is rejected"), FAkUGCTowerDefensePathBuilder::Build(Scene).bSucceeded);

    Scene.Entities[1] = MakePathNode(FGuid(2, 0, 0, 0), 20, FVector(100.0, 0.0, 0.0));
    Scene.Entities[1].Components[0].Properties.Remove(TEXT("order"));
    TestFalse(TEXT("Missing path order is rejected"), FAkUGCTowerDefensePathBuilder::Build(Scene).bSucceeded);

    Scene.Entities[1] = MakePathNode(FGuid(2, 0, 0, 0), 20, FVector(0.5, 0.0, 0.0));
    TestFalse(TEXT("Sub-centimeter path segment is rejected"), FAkUGCTowerDefensePathBuilder::Build(Scene).bSucceeded);

    Scene.Entities[1] = MakePathNode(Scene.Entities[0].EntityId, 20, FVector(100.0, 0.0, 0.0));
    TestFalse(TEXT("Duplicate path Entity ID is rejected"), FAkUGCTowerDefensePathBuilder::Build(Scene).bSucceeded);

    Scene.Entities[1] = MakePathNode(FGuid(2, 0, 0, 0), 20, FVector(100.0, 0.0, 0.0));
    const FAkUGCComponentRecord DuplicateComponent = Scene.Entities[1].Components[0];
    Scene.Entities[1].Components.Add(DuplicateComponent);
    TestFalse(TEXT("Duplicate path component is rejected"), FAkUGCTowerDefensePathBuilder::Build(Scene).bSucceeded);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCTowerDefensePathRuntimeIntegrationTest,
    "AkUGC.Runtime.Path.SessionProjection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCTowerDefensePathRuntimeIntegrationTest::RunTest(const FString& Parameters)
{
    if (!GEngine)
    {
        AddError(TEXT("Engine is not available."));
        return false;
    }

    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AkUGCTowerDefensePathRuntimeTest"));
    if (!World)
    {
        AddError(TEXT("Failed to create path runtime test world."));
        return false;
    }
    FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
    WorldContext.SetCurrentWorld(World);

    FAkUGCPrefabRegistry Registry;
    FString Error;
    TestTrue(TEXT("Official prefabs register for path runtime"),
        FAkUGCOfficialPrefabCatalog::RegisterTowerDefense(Registry, &Error));

    FAkUGCProjectDocument Document;
    Document.Manifest.ProjectId = FGuid::NewGuid();
    Document.Manifest.DisplayName = TEXT("Path Runtime Test");
    Document.Manifest.TemplateId = TEXT("official.tower_defense");
    FAkUGCSceneDocument& Scene = Document.Scenes.AddDefaulted_GetRef();
    Scene.SceneId = FGuid::NewGuid();
    Scene.DisplayName = TEXT("Main");

    const FGuid FirstId = FGuid::NewGuid();
    const FGuid SecondId = FGuid::NewGuid();
    FAkUGCEntityRecord First;
    FAkUGCEntityRecord Second;
    TestTrue(TEXT("First official path node is created"), Registry.CreateEntityRecord(
        TEXT("official.gameplay.path_node"), FirstId, FTransform(FVector(100.0, 0.0, 0.0)), First, &Error));
    TestTrue(TEXT("Second official path node is created"), Registry.CreateEntityRecord(
        TEXT("official.gameplay.path_node"), SecondId, FTransform(FVector(200.0, 0.0, 0.0)), Second, &Error));
    First.Components[0].Properties.FindChecked(TEXT("order")).IntegerValue = 10;
    Second.Components[0].Properties.FindChecked(TEXT("order")).IntegerValue = 20;
    Scene.Entities = {Second, First};

    {
        FAkUGCSceneRuntime Runtime(World);
        FAkUGCDocumentRuntimeSession Session(Runtime, Registry, Scene.SceneId);
        TestTrue(TEXT("Edit session builds derived path"), Session.Initialize(Document).bSucceeded);
        TestEqual(TEXT("Runtime exposes two ordered path nodes"), Runtime.GetTowerDefensePath().Num(), 2);
        if (Runtime.GetTowerDefensePath().Num() == 2)
        {
            TestEqual(TEXT("Runtime path starts at lower order"), Runtime.GetTowerDefensePath().Nodes[0].EntityId, FirstId);
        }

        FAkUGCCommand ChangeOrder;
        ChangeOrder.CommandId = FGuid::NewGuid();
        ChangeOrder.Type = EAkUGCCommandType::SetProperty;
        ChangeOrder.SceneId = Scene.SceneId;
        ChangeOrder.EntityId = FirstId;
        ChangeOrder.ComponentTypeId = TEXT("tower_defense.path_node");
        ChangeOrder.PropertyId = TEXT("order");
        ChangeOrder.PropertyValue.Type = EAkUGCValueType::Integer;
        ChangeOrder.PropertyValue.IntegerValue = 30;
        FAkUGCCommandTransaction Transaction;
        Transaction.TransactionId = FGuid::NewGuid();
        Transaction.Label = TEXT("Reorder path node");
        Transaction.Commands.Add(ChangeOrder);
        TestTrue(TEXT("Path order transaction succeeds"), Session.Execute(Document, Transaction).bSucceeded);
        TestEqual(TEXT("Runtime path updates after Command"), Runtime.GetTowerDefensePath().Nodes[0].EntityId, SecondId);
        TestTrue(TEXT("Undo path order succeeds"), Session.Undo(Document).bSucceeded);
        TestEqual(TEXT("Undo restores derived path order"), Runtime.GetTowerDefensePath().Nodes[0].EntityId, FirstId);
    }

    GEngine->DestroyWorldContext(World);
    World->DestroyWorld(false);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCTowerDefensePathEditingWorkflowTest,
    "AkUGC.Runtime.Path.EditingWorkflow",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCTowerDefensePathEditingWorkflowTest::RunTest(const FString& Parameters)
{
    if (!GEngine)
    {
        AddError(TEXT("Engine is not available."));
        return false;
    }

    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AkUGCTowerDefensePathEditingTest"));
    if (!World)
    {
        AddError(TEXT("Failed to create path editing test world."));
        return false;
    }
    FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
    WorldContext.SetCurrentWorld(World);

    FAkUGCPrefabRegistry Registry;
    FString Error;
    TestTrue(TEXT("Official prefabs register for path editing"),
        FAkUGCOfficialPrefabCatalog::RegisterTowerDefense(Registry, &Error));

    FGuid SceneId;
    FAkUGCProjectDocument Document;
    Document.Manifest.ProjectId = FGuid::NewGuid();
    Document.Manifest.DisplayName = TEXT("Path Editing Test");
    Document.Manifest.TemplateId = TEXT("official.tower_defense");
    FAkUGCSceneDocument& Scene = Document.Scenes.AddDefaulted_GetRef();
    Scene.SceneId = FGuid::NewGuid();
    Scene.DisplayName = TEXT("Main");
    SceneId = Scene.SceneId;

    {
        FAkUGCSceneRuntime Runtime(World);
        FAkUGCDocumentRuntimeSession Session(Runtime, Registry, SceneId);
        TestTrue(TEXT("Path editing session initializes"), Session.Initialize(Document).bSucceeded);
        FAkUGCRuntimeCommandService Service(Document, Session, Registry, SceneId);

        FGuid FirstId;
        FGuid SecondId;
        FGuid DuplicateId;
        TestTrue(TEXT("First path node placement succeeds"), Service.PlacePrefab(
            TEXT("official.gameplay.path_node"), FTransform(FVector(100.0, 0.0, 0.0)), FirstId).bSucceeded);
        TestTrue(TEXT("Second path node placement succeeds"), Service.PlacePrefab(
            TEXT("official.gameplay.path_node"), FTransform(FVector(200.0, 0.0, 0.0)), SecondId).bSucceeded);
        TestTrue(TEXT("Path node duplication succeeds"), Service.DuplicateEntity(
            SecondId, FVector(100.0, 0.0, 0.0), DuplicateId).bSucceeded);
        TestEqual(TEXT("Three path nodes are derived after editing"), Runtime.GetTowerDefensePath().Num(), 3);
        if (Runtime.GetTowerDefensePath().Num() == 3)
        {
            TestEqual(TEXT("First placed path node receives order zero"), Runtime.GetTowerDefensePath().Nodes[0].Order, int64(0));
            TestEqual(TEXT("Second placed path node receives order one"), Runtime.GetTowerDefensePath().Nodes[1].Order, int64(1));
            TestEqual(TEXT("Duplicated path node receives order two"), Runtime.GetTowerDefensePath().Nodes[2].Order, int64(2));
        }
        TestTrue(TEXT("Undo duplicated path node succeeds"), Service.Undo().bSucceeded);
        TestEqual(TEXT("Undo restores two-node path"), Runtime.GetTowerDefensePath().Num(), 2);
        TestTrue(TEXT("Redo duplicated path node succeeds"), Service.Redo().bSucceeded);
        TestEqual(TEXT("Redo restores three-node path"), Runtime.GetTowerDefensePath().Num(), 3);
    }

    GEngine->DestroyWorldContext(World);
    World->DestroyWorld(false);
    return true;
}

#endif

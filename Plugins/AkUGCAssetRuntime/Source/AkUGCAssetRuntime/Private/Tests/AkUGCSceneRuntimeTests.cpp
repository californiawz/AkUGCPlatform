#include "Misc/AutomationTest.h"

#include "Document/AkUGCDocument.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Entity/AkUGCEntityBindingComponent.h"
#include "Prefab/AkUGCPrefabRegistry.h"
#include "Scene/AkUGCSceneRuntime.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
    FAkUGCPrefabDefinition MakePrefab(FName PrefabId)
    {
        FAkUGCPrefabDefinition Definition;
        Definition.PrefabId = PrefabId;
        Definition.DisplayName = PrefabId.ToString();
        Definition.EntityType = TEXT("Test.Entity");
        return Definition;
    }

    FAkUGCEntityRecord MakeEntity(FName PrefabId, const FVector& Location)
    {
        FAkUGCEntityRecord Entity;
        Entity.EntityId = FGuid::NewGuid();
        Entity.PrefabId = PrefabId;
        Entity.Transform.SetLocation(Location);
        return Entity;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCSceneRuntimeLifecycleTest,
    "AkUGC.Runtime.Scene.Lifecycle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCSceneRuntimeLifecycleTest::RunTest(const FString& Parameters)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AkUGCSceneRuntimeTest"));
    TestNotNull(TEXT("Test world is created"), World);
    if (!World)
    {
        return false;
    }

    FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
    WorldContext.SetCurrentWorld(World);

    FAkUGCPrefabRegistry Registry;
    FString Error;
    TestTrue(TEXT("Base prefab registers"), Registry.Register(MakePrefab(TEXT("official.gameplay.base")), &Error));
    TestTrue(TEXT("Tower prefab registers"), Registry.Register(MakePrefab(TEXT("official.gameplay.tower")), &Error));

    FAkUGCSceneDocument Scene;
    Scene.SceneId = FGuid::NewGuid();
    Scene.DisplayName = TEXT("Runtime Test");
    FAkUGCEntityRecord Parent = MakeEntity(TEXT("official.gameplay.base"), FVector(100.0, 0.0, 0.0));
    FAkUGCEntityRecord Child = MakeEntity(TEXT("official.gameplay.tower"), FVector(150.0, 0.0, 0.0));
    Child.ParentEntityId = Parent.EntityId;
    Scene.Entities = {Parent, Child};

    {
        FAkUGCSceneRuntime Runtime(World);
        TestTrue(TEXT("Scene loads"), Runtime.LoadScene(Scene, Registry, &Error));
        TestEqual(TEXT("Two runtime actors are spawned"), Runtime.Num(), 2);

        AActor* ParentActor = Runtime.FindActor(Parent.EntityId);
        AActor* ChildActor = Runtime.FindActor(Child.EntityId);
        TestNotNull(TEXT("Parent actor exists"), ParentActor);
        TestNotNull(TEXT("Child actor exists"), ChildActor);
        if (ParentActor && ChildActor)
        {
            TestEqual(TEXT("Parent transform is applied"), ParentActor->GetActorLocation(), FVector(100.0, 0.0, 0.0));
            TestEqual(TEXT("Child is attached"), ChildActor->GetAttachParentActor(), ParentActor);
            const UAkUGCEntityBindingComponent* Binding = ChildActor->FindComponentByClass<UAkUGCEntityBindingComponent>();
            TestNotNull(TEXT("Binding component exists"), Binding);
        }

        Child.Transform.SetLocation(FVector(300.0, 0.0, 0.0));
        TestTrue(TEXT("Entity update succeeds"), Runtime.ApplyEntity(Child, Registry, &Error));
        TestEqual(TEXT("Entity update changes transform"), ChildActor->GetActorLocation(), FVector(300.0, 0.0, 0.0));

        TestTrue(TEXT("Entity removal succeeds"), Runtime.RemoveEntity(Child.EntityId));
        TestEqual(TEXT("One runtime actor remains"), Runtime.Num(), 1);
        Runtime.Unload();
        TestEqual(TEXT("Unload removes all runtime actors"), Runtime.Num(), 0);
    }

    GEngine->DestroyWorldContext(World);
    World->DestroyWorld(false);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCSceneRuntimeValidationTest,
    "AkUGC.Runtime.Scene.RejectsUnknownPrefab",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCSceneRuntimeValidationTest::RunTest(const FString& Parameters)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AkUGCSceneValidationTest"));
    if (!World)
    {
        AddError(TEXT("Failed to create test world."));
        return false;
    }

    FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
    WorldContext.SetCurrentWorld(World);

    FAkUGCPrefabRegistry Registry;
    FAkUGCSceneDocument Scene;
    Scene.SceneId = FGuid::NewGuid();
    Scene.Entities.Add(MakeEntity(TEXT("missing.prefab"), FVector::ZeroVector));

    FAkUGCSceneRuntime Runtime(World);
    FString Error;
    TestFalse(TEXT("Unknown prefab is rejected"), Runtime.LoadScene(Scene, Registry, &Error));
    TestEqual(TEXT("Rejected scene spawns no actors"), Runtime.Num(), 0);
    TestFalse(TEXT("Validation returns an error"), Error.IsEmpty());

    GEngine->DestroyWorldContext(World);
    World->DestroyWorld(false);
    return true;
}

#endif

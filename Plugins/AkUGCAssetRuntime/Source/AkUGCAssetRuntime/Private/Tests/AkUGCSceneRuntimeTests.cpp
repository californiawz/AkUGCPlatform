#include "Misc/AutomationTest.h"

#include "Document/AkUGCDocument.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Prefab/AkUGCOfficialPrefabCatalog.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Entity/AkUGCEntityBindingComponent.h"
#include "Entity/AkUGCRuntimeEntityActor.h"
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
    Scene.Entities = {Child, Parent};

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCSceneRuntimeParentCycleTest,
    "AkUGC.Runtime.Scene.RejectsParentCycles",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCSceneRuntimeParentCycleTest::RunTest(const FString& Parameters)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AkUGCSceneParentCycleTest"));
    if (!World)
    {
        AddError(TEXT("Failed to create test world."));
        return false;
    }
    FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
    WorldContext.SetCurrentWorld(World);

    FAkUGCPrefabRegistry Registry;
    FString Error;
    Registry.Register(MakePrefab(TEXT("official.gameplay.base")), &Error);

    FAkUGCSceneDocument Scene;
    Scene.SceneId = FGuid::NewGuid();
    FAkUGCEntityRecord First = MakeEntity(TEXT("official.gameplay.base"), FVector::ZeroVector);
    FAkUGCEntityRecord Second = MakeEntity(TEXT("official.gameplay.base"), FVector(100.0, 0.0, 0.0));
    First.ParentEntityId = Second.EntityId;
    Second.ParentEntityId = First.EntityId;
    Scene.Entities = {First, Second};

    FAkUGCSceneRuntime Runtime(World);
    TestFalse(TEXT("Runtime rejects parent cycle before spawning"), Runtime.LoadScene(Scene, Registry, &Error));
    TestEqual(TEXT("Rejected cycle spawns no actors"), Runtime.Num(), 0);
    TestFalse(TEXT("Cycle rejection returns an error"), Error.IsEmpty());

    GEngine->DestroyWorldContext(World);
    World->DestroyWorld(false);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCSceneRuntimeEntityIdentityTest,
    "AkUGC.Runtime.Scene.ReplicatesEntityIdentity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCSceneRuntimeEntityIdentityTest::RunTest(const FString& Parameters)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AkUGCSceneEntityIdentityTest"));
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

    const FAkUGCEntityRecord Entity = MakeEntity(TEXT("official.gameplay.base"), FVector(50.0, 0.0, 0.0));
    FAkUGCSceneDocument Scene;
    Scene.SceneId = FGuid::NewGuid();
    Scene.Entities = {Entity};

    {
        FAkUGCSceneRuntime Runtime(World);
        TestTrue(TEXT("Scene loads"), Runtime.LoadScene(Scene, Registry, &Error));

        AActor* Actor = Runtime.FindActor(Entity.EntityId);
        TestNotNull(TEXT("Runtime actor exists"), Actor);
        if (!Actor)
        {
            GEngine->DestroyWorldContext(World);
            World->DestroyWorld(false);
            return false;
        }

        TestTrue(TEXT("Runtime actor is network replicated"), Actor->GetIsReplicated());

        const AAkUGCRuntimeEntityActor* RuntimeEntity = Cast<AAkUGCRuntimeEntityActor>(Actor);
        TestNotNull(TEXT("Actor is a runtime entity"), RuntimeEntity);
        if (RuntimeEntity)
        {
            TestEqual(TEXT("Replicated EntityId matches document"), RuntimeEntity->GetEntityId(), Entity.EntityId);
            TestEqual(TEXT("Replicated PrefabId matches document"), RuntimeEntity->GetPrefabId(), Entity.PrefabId);
        }

        // 销毁协议：权威端移除实体后，Actor 销毁并退出场景映射。
        TestTrue(TEXT("Entity removal succeeds"), Runtime.RemoveEntity(Entity.EntityId));
        TestEqual(TEXT("Removed entity leaves the actor map"), Runtime.Num(), 0);
    }

    GEngine->DestroyWorldContext(World);
    World->DestroyWorld(false);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCReplicatedPrefabVisualTest,
    "AkUGC.Runtime.Scene.OfficialPrefabVisuals",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
        | EAutomationTestFlags::ServerContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCReplicatedPrefabVisualTest::RunTest(const FString& Parameters)
{
    FAkUGCPrefabRegistry Registry;
    FString Error;
    if (!FAkUGCOfficialPrefabCatalog::RegisterTowerDefense(Registry, &Error))
    {
        AddError(Error);
        return false;
    }
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AkUGCVisualTest"));
    if (!World) { return false; }
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    const TArray<FName> Prefabs = {TEXT("official.gameplay.base"), TEXT("official.unit.basic_enemy"),
        TEXT("official.tower.basic"), TEXT("official.gameplay.enemy_spawn"),
        TEXT("official.gameplay.path_node"), TEXT("official.gameplay.goal")};
    {
        FAkUGCSceneDocument Scene;
        Scene.SceneId = FGuid::NewGuid();
        for (FName Id : Prefabs)
        {
            const FSoftObjectPath* Desktop = Registry.ResolveAsset(Id, TEXT("Win64"));
            const FSoftObjectPath* Mobile = Registry.ResolveAsset(Id, TEXT("Android"));
            const FSoftObjectPath* Server = Registry.ResolveAsset(Id, TEXT("Server"));
            TestTrue(TEXT("Win64 placeholder path exists"), Desktop && Desktop->IsValid());
            TestTrue(TEXT("Android placeholder path exists"), Mobile && Mobile->IsValid());
            TestTrue(TEXT("Server explicitly has no visual path"), Server && !Server->IsValid());
            TestTrue(TEXT("Visible placeholder has nonzero render cost"), Registry.Find(Id)->Cost.RenderUnits > 0);
            FAkUGCEntityRecord Entity;
            if (Registry.CreateEntityRecord(Id, FGuid::NewGuid(), FTransform::Identity, Entity, &Error))
            {
                Scene.Entities.Add(Entity);
            }
            else { AddError(Error); }
        }
        FAkUGCSceneRuntime Runtime(World);
        if (!Runtime.LoadScene(Scene, Registry, &Error)) { AddError(Error); }
        for (const FAkUGCEntityRecord& Entity : Scene.Entities)
        {
            AAkUGCRuntimeEntityActor* Actor = Cast<AAkUGCRuntimeEntityActor>(Runtime.FindActor(Entity.EntityId));
            if (!Actor) { AddError(TEXT("Missing visual runtime entity.")); continue; }
#if UE_SERVER
            TestNull(TEXT("Dedicated Server avoids visual Mesh loading"), Actor->VisualMesh->GetStaticMesh().Get());
#else
            TestNotNull(TEXT("Official placeholder StaticMesh loads"), Actor->VisualMesh->GetStaticMesh().Get());
            Actor->SetVisualMesh(nullptr);
            Actor->OnRep_PrefabId();
            TestNotNull(TEXT("Prefab replication callback restores platform Mesh"), Actor->VisualMesh->GetStaticMesh().Get());
            Actor->PrefabId = NAME_None;
            Actor->OnRep_PrefabId();
            TestNull(TEXT("Empty replicated Prefab clears stale visual"), Actor->VisualMesh->GetStaticMesh().Get());
#endif
        }
    }
    GEngine->DestroyWorldContext(World);
    World->DestroyWorld(false);
    return true;
}

#endif

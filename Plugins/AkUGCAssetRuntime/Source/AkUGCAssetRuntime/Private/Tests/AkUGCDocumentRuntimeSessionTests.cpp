#include "Misc/AutomationTest.h"

#include "Command/AkUGCCommand.h"
#include "Document/AkUGCDocument.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Prefab/AkUGCPrefabRegistry.h"
#include "Scene/AkUGCSceneRuntime.h"
#include "Session/AkUGCDocumentRuntimeSession.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
    FAkUGCProjectDocument MakeSessionDocument(FGuid& OutSceneId)
    {
        FAkUGCProjectDocument Document;
        Document.Manifest.ProjectId = FGuid::NewGuid();
        Document.Manifest.DisplayName = TEXT("Runtime Session Test");
        Document.Manifest.TemplateId = TEXT("official.tower_defense");

        FAkUGCSceneDocument& Scene = Document.Scenes.AddDefaulted_GetRef();
        Scene.SceneId = FGuid::NewGuid();
        Scene.DisplayName = TEXT("Main");
        OutSceneId = Scene.SceneId;
        return Document;
    }

    FAkUGCPrefabDefinition MakeSessionPrefab(FName PrefabId)
    {
        FAkUGCPrefabDefinition Definition;
        Definition.PrefabId = PrefabId;
        Definition.DisplayName = PrefabId.ToString();
        Definition.EntityType = TEXT("Test.Entity");
        return Definition;
    }

    FAkUGCCommand MakeSessionCommand(EAkUGCCommandType Type, FGuid SceneId, FGuid EntityId)
    {
        FAkUGCCommand Command;
        Command.CommandId = FGuid::NewGuid();
        Command.Type = Type;
        Command.SceneId = SceneId;
        Command.EntityId = EntityId;
        return Command;
    }

    FAkUGCCommandTransaction MakeSessionTransaction(FString Label, TArray<FAkUGCCommand> Commands)
    {
        FAkUGCCommandTransaction Transaction;
        Transaction.TransactionId = FGuid::NewGuid();
        Transaction.Label = MoveTemp(Label);
        Transaction.Commands = MoveTemp(Commands);
        return Transaction;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCDocumentRuntimeSessionTest,
    "AkUGC.Runtime.Session.CommandUndoRedoProjection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCDocumentRuntimeSessionTest::RunTest(const FString& Parameters)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AkUGCDocumentRuntimeSessionTest"));
    if (!World)
    {
        AddError(TEXT("Failed to create test world."));
        return false;
    }
    FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
    WorldContext.SetCurrentWorld(World);

    FGuid SceneId;
    FAkUGCProjectDocument Document = MakeSessionDocument(SceneId);
    FAkUGCPrefabRegistry Registry;
    FString RegistryError;
    TestTrue(
        TEXT("Session prefab registers"),
        Registry.Register(MakeSessionPrefab(TEXT("official.gameplay.base")), &RegistryError));

    {
        FAkUGCSceneRuntime Runtime(World);
        FAkUGCDocumentRuntimeSession Session(Runtime, Registry, SceneId);
        TestTrue(TEXT("Session initializes"), Session.Initialize(Document).bSucceeded);

        const FGuid EntityId = FGuid::NewGuid();
        FAkUGCCommand Add = MakeSessionCommand(EAkUGCCommandType::AddEntity, SceneId, EntityId);
        Add.Entity.EntityId = EntityId;
        Add.Entity.PrefabId = TEXT("official.gameplay.base");
        Add.Entity.Transform.SetLocation(FVector(10.0, 0.0, 0.0));
        TestTrue(
            TEXT("Add command updates document and runtime"),
            Session.Execute(Document, MakeSessionTransaction(TEXT("Add base"), {Add})).bSucceeded);
        TestEqual(TEXT("Document contains added entity"), Document.Scenes[0].Entities.Num(), 1);
        TestNotNull(TEXT("Runtime contains added actor"), Runtime.FindActor(EntityId));

        FAkUGCCommand Move = MakeSessionCommand(EAkUGCCommandType::SetTransform, SceneId, EntityId);
        Move.Transform.SetLocation(FVector(20.0, 0.0, 0.0));
        TestTrue(
            TEXT("Move command updates document and runtime"),
            Session.Execute(Document, MakeSessionTransaction(TEXT("Move base"), {Move})).bSucceeded);
        TestEqual(TEXT("Runtime actor moved"), Runtime.FindActor(EntityId)->GetActorLocation(), FVector(20.0, 0.0, 0.0));

        TestTrue(TEXT("Undo move succeeds"), Session.Undo(Document).bSucceeded);
        TestEqual(TEXT("Undo restores runtime transform"), Runtime.FindActor(EntityId)->GetActorLocation(), FVector(10.0, 0.0, 0.0));
        TestTrue(TEXT("Undo add succeeds"), Session.Undo(Document).bSucceeded);
        TestEqual(TEXT("Undo add removes document entity"), Document.Scenes[0].Entities.Num(), 0);
        TestNull(TEXT("Undo add removes runtime actor"), Runtime.FindActor(EntityId));

        TestTrue(TEXT("Redo add succeeds"), Session.Redo(Document).bSucceeded);
        TestNotNull(TEXT("Redo add restores runtime actor"), Runtime.FindActor(EntityId));
        TestTrue(TEXT("Redo move succeeds"), Session.Redo(Document).bSucceeded);
        TestEqual(TEXT("Redo move restores runtime transform"), Runtime.FindActor(EntityId)->GetActorLocation(), FVector(20.0, 0.0, 0.0));
        Runtime.Unload();
    }

    GEngine->DestroyWorldContext(World);
    World->DestroyWorld(false);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCDocumentRuntimeProjectionRollbackTest,
    "AkUGC.Runtime.Session.ProjectionFailureRollsBackDocument",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCDocumentRuntimeProjectionRollbackTest::RunTest(const FString& Parameters)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AkUGCProjectionRollbackTest"));
    if (!World)
    {
        AddError(TEXT("Failed to create test world."));
        return false;
    }
    FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
    WorldContext.SetCurrentWorld(World);

    FGuid SceneId;
    FAkUGCProjectDocument Document = MakeSessionDocument(SceneId);
    FAkUGCPrefabRegistry Registry;

    {
        FAkUGCSceneRuntime Runtime(World);
        FAkUGCDocumentRuntimeSession Session(Runtime, Registry, SceneId);
        TestTrue(TEXT("Empty session initializes"), Session.Initialize(Document).bSucceeded);

        const FGuid EntityId = FGuid::NewGuid();
        FAkUGCCommand Add = MakeSessionCommand(EAkUGCCommandType::AddEntity, SceneId, EntityId);
        Add.Entity.EntityId = EntityId;
        Add.Entity.PrefabId = TEXT("missing.prefab");

        const FAkUGCCommandExecutionResult Result = Session.Execute(
            Document,
            MakeSessionTransaction(TEXT("Add invalid prefab"), {Add}));
        TestFalse(TEXT("Runtime projection failure rejects transaction"), Result.bSucceeded);
        TestEqual(TEXT("Document is rolled back"), Document.Scenes[0].Entities.Num(), 0);
        TestEqual(TEXT("Runtime remains unchanged"), Runtime.Num(), 0);
        TestFalse(TEXT("Rejected transaction is not added to undo history"), Session.CanUndo());
    }

    GEngine->DestroyWorldContext(World);
    World->DestroyWorld(false);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCDocumentRuntimeDeltaProjectionTest,
    "AkUGC.Runtime.Session.OnlyTouchesCommandEntities",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCDocumentRuntimeDeltaProjectionTest::RunTest(const FString& Parameters)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AkUGCDeltaProjectionTest"));
    if (!World)
    {
        AddError(TEXT("Failed to create test world."));
        return false;
    }
    FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
    WorldContext.SetCurrentWorld(World);

    FGuid SceneId;
    FAkUGCProjectDocument Document = MakeSessionDocument(SceneId);
    FAkUGCPrefabRegistry Registry;
    FString RegistryError;
    Registry.Register(MakeSessionPrefab(TEXT("official.gameplay.base")), &RegistryError);

    const FGuid ChangedEntityId = FGuid::NewGuid();
    FAkUGCEntityRecord ChangedEntity;
    ChangedEntity.EntityId = ChangedEntityId;
    ChangedEntity.PrefabId = TEXT("official.gameplay.base");
    ChangedEntity.Transform.SetLocation(FVector(10.0, 0.0, 0.0));

    const FGuid UntouchedEntityId = FGuid::NewGuid();
    FAkUGCEntityRecord UntouchedEntity;
    UntouchedEntity.EntityId = UntouchedEntityId;
    UntouchedEntity.PrefabId = TEXT("official.gameplay.base");
    UntouchedEntity.Transform.SetLocation(FVector(50.0, 0.0, 0.0));

    Document.Scenes[0].Entities = {ChangedEntity, UntouchedEntity};

    {
        FAkUGCSceneRuntime Runtime(World);
        FAkUGCDocumentRuntimeSession Session(Runtime, Registry, SceneId);
        TestTrue(TEXT("Delta session initializes"), Session.Initialize(Document).bSucceeded);

        AActor* UntouchedActor = Runtime.FindActor(UntouchedEntityId);
        TestNotNull(TEXT("Untouched actor exists"), UntouchedActor);
        UntouchedActor->SetActorLocation(FVector(999.0, 0.0, 0.0));

        FAkUGCCommand Move = MakeSessionCommand(EAkUGCCommandType::SetTransform, SceneId, ChangedEntityId);
        Move.Transform.SetLocation(FVector(200.0, 0.0, 0.0));
        TestTrue(
            TEXT("Delta command succeeds"),
            Session.Execute(Document, MakeSessionTransaction(TEXT("Move one entity"), {Move})).bSucceeded);

        TestEqual(
            TEXT("Changed actor is updated"),
            Runtime.FindActor(ChangedEntityId)->GetActorLocation(),
            FVector(200.0, 0.0, 0.0));
        TestEqual(
            TEXT("Untouched actor is not refreshed"),
            UntouchedActor->GetActorLocation(),
            FVector(999.0, 0.0, 0.0));
        Runtime.Unload();
    }

    GEngine->DestroyWorldContext(World);
    World->DestroyWorld(false);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCDocumentRuntimeAddDeleteTransactionTest,
    "AkUGC.Runtime.Session.AddThenDeleteSkipsAttachment",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCDocumentRuntimeAddDeleteTransactionTest::RunTest(const FString& Parameters)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AkUGCAddDeleteTransactionTest"));
    if (!World)
    {
        AddError(TEXT("Failed to create test world."));
        return false;
    }
    FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
    WorldContext.SetCurrentWorld(World);

    FGuid SceneId;
    FAkUGCProjectDocument Document = MakeSessionDocument(SceneId);
    FAkUGCPrefabRegistry Registry;
    FString RegistryError;
    Registry.Register(MakeSessionPrefab(TEXT("official.gameplay.base")), &RegistryError);

    {
        FAkUGCSceneRuntime Runtime(World);
        FAkUGCDocumentRuntimeSession Session(Runtime, Registry, SceneId);
        TestTrue(TEXT("Add-delete session initializes"), Session.Initialize(Document).bSucceeded);

        const FGuid EntityId = FGuid::NewGuid();
        FAkUGCCommand Add = MakeSessionCommand(EAkUGCCommandType::AddEntity, SceneId, EntityId);
        Add.Entity.EntityId = EntityId;
        Add.Entity.PrefabId = TEXT("official.gameplay.base");
        FAkUGCCommand Delete = MakeSessionCommand(EAkUGCCommandType::DeleteEntity, SceneId, EntityId);

        TestTrue(
            TEXT("Add then delete transaction succeeds"),
            Session.Execute(Document, MakeSessionTransaction(TEXT("Transient entity"), {Add, Delete})).bSucceeded);
        TestEqual(TEXT("Document final state has no entity"), Document.Scenes[0].Entities.Num(), 0);
        TestEqual(TEXT("Runtime final state has no actor"), Runtime.Num(), 0);
        Runtime.Unload();
    }

    GEngine->DestroyWorldContext(World);
    World->DestroyWorld(false);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCDocumentRuntimeReplaceParentTest,
    "AkUGC.Runtime.Session.ReplaceParentReattachesChildren",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCDocumentRuntimeReplaceParentTest::RunTest(const FString& Parameters)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AkUGCReplaceParentTest"));
    if (!World)
    {
        AddError(TEXT("Failed to create test world."));
        return false;
    }
    FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
    WorldContext.SetCurrentWorld(World);

    FGuid SceneId;
    FAkUGCProjectDocument Document = MakeSessionDocument(SceneId);
    FAkUGCPrefabRegistry Registry;
    FString RegistryError;
    Registry.Register(MakeSessionPrefab(TEXT("official.gameplay.base")), &RegistryError);

    const FGuid ParentId = FGuid::NewGuid();
    FAkUGCEntityRecord Parent;
    Parent.EntityId = ParentId;
    Parent.PrefabId = TEXT("official.gameplay.base");

    const FGuid ChildId = FGuid::NewGuid();
    FAkUGCEntityRecord Child;
    Child.EntityId = ChildId;
    Child.PrefabId = TEXT("official.gameplay.base");
    Child.ParentEntityId = ParentId;
    Document.Scenes[0].Entities = {Child, Parent};

    {
        FAkUGCSceneRuntime Runtime(World);
        FAkUGCDocumentRuntimeSession Session(Runtime, Registry, SceneId);
        TestTrue(TEXT("Replace-parent session initializes"), Session.Initialize(Document).bSucceeded);

        FAkUGCCommand DeleteParent = MakeSessionCommand(EAkUGCCommandType::DeleteEntity, SceneId, ParentId);
        FAkUGCCommand AddParent = MakeSessionCommand(EAkUGCCommandType::AddEntity, SceneId, ParentId);
        AddParent.Entity = Parent;
        AddParent.Entity.Transform.SetLocation(FVector(500.0, 0.0, 0.0));

        TestTrue(
            TEXT("Parent replacement transaction succeeds"),
            Session.Execute(
                Document,
                MakeSessionTransaction(TEXT("Replace parent"), {DeleteParent, AddParent})).bSucceeded);

        AActor* ChildActor = Runtime.FindActor(ChildId);
        AActor* ReplacementParentActor = Runtime.FindActor(ParentId);
        TestNotNull(TEXT("Child actor survives parent replacement"), ChildActor);
        TestNotNull(TEXT("Replacement parent actor exists"), ReplacementParentActor);
        if (ChildActor && ReplacementParentActor)
        {
            TestEqual(TEXT("Child is reattached to replacement parent"), ChildActor->GetAttachParentActor(), ReplacementParentActor);
        }
        Runtime.Unload();
    }

    GEngine->DestroyWorldContext(World);
    World->DestroyWorld(false);
    return true;
}

#endif

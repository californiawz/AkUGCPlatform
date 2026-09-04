#include "Misc/AutomationTest.h"

#include "Command/AkUGCCommandHistory.h"
#include "Command/AkUGCCommandJson.h"
#include "Document/AkUGCDocument.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
    FAkUGCProjectDocument MakeDocument(FGuid& OutSceneId)
    {
        FAkUGCProjectDocument Document;
        Document.Manifest.ProjectId = FGuid::NewGuid();
        Document.Manifest.DisplayName = TEXT("Command Test");
        Document.Manifest.TemplateId = TEXT("official.tower_defense");

        FAkUGCSceneDocument& Scene = Document.Scenes.AddDefaulted_GetRef();
        Scene.SceneId = FGuid::NewGuid();
        Scene.DisplayName = TEXT("Main");
        OutSceneId = Scene.SceneId;
        return Document;
    }

    FAkUGCEntityRecord MakeEntity(const FGuid& EntityId)
    {
        FAkUGCEntityRecord Entity;
        Entity.EntityId = EntityId;
        Entity.PrefabId = TEXT("official.gameplay.base");

        FAkUGCComponentRecord& Health = Entity.Components.AddDefaulted_GetRef();
        Health.TypeId = TEXT("core.health");
        return Entity;
    }

    FAkUGCCommand MakeCommand(EAkUGCCommandType Type, const FGuid& SceneId, const FGuid& EntityId)
    {
        FAkUGCCommand Command;
        Command.CommandId = FGuid::NewGuid();
        Command.Type = Type;
        Command.SceneId = SceneId;
        Command.EntityId = EntityId;
        return Command;
    }

    FAkUGCCommandTransaction MakeTransaction(FString Label, TArray<FAkUGCCommand> Commands)
    {
        FAkUGCCommandTransaction Transaction;
        Transaction.TransactionId = FGuid::NewGuid();
        Transaction.Label = MoveTemp(Label);
        Transaction.Commands = MoveTemp(Commands);
        return Transaction;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCCommandUndoRedoTest,
    "AkUGC.Core.Command.UndoRedo",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCCommandUndoRedoTest::RunTest(const FString& Parameters)
{
    FGuid SceneId;
    FAkUGCProjectDocument Document = MakeDocument(SceneId);
    const FGuid EntityId = FGuid::NewGuid();

    FAkUGCCommand Add = MakeCommand(EAkUGCCommandType::AddEntity, SceneId, EntityId);
    Add.Entity = MakeEntity(EntityId);

    FAkUGCCommand Transform = MakeCommand(EAkUGCCommandType::SetTransform, SceneId, EntityId);
    Transform.Transform = FTransform(FVector(100.0, 200.0, 300.0));

    FAkUGCCommand Property = MakeCommand(EAkUGCCommandType::SetProperty, SceneId, EntityId);
    Property.ComponentTypeId = TEXT("core.health");
    Property.PropertyId = TEXT("maxHealth");
    Property.PropertyValue.Type = EAkUGCValueType::Number;
    Property.PropertyValue.NumberValue = 1000.0;

    FAkUGCCommandHistory History;
    const FAkUGCCommandTransaction Transaction = MakeTransaction(TEXT("Create base"), {Add, Transform, Property});
    TestTrue(TEXT("Transaction succeeds"), History.Execute(Document, Transaction).bSucceeded);
    TestEqual(TEXT("Entity is added"), Document.Scenes[0].Entities.Num(), 1);
    TestEqual(
        TEXT("Transform is applied"),
        Document.Scenes[0].Entities[0].Transform.GetLocation(),
        FVector(100.0, 200.0, 300.0));
    TestTrue(TEXT("Undo is available"), History.CanUndo());

    TestTrue(TEXT("Undo succeeds"), History.Undo(Document).bSucceeded);
    TestEqual(TEXT("Undo removes entity"), Document.Scenes[0].Entities.Num(), 0);
    TestTrue(TEXT("Redo is available"), History.CanRedo());

    TestTrue(TEXT("Redo succeeds"), History.Redo(Document).bSucceeded);
    TestEqual(TEXT("Redo restores entity"), Document.Scenes[0].Entities.Num(), 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCCommandAtomicRollbackTest,
    "AkUGC.Core.Command.AtomicRollback",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCCommandAtomicRollbackTest::RunTest(const FString& Parameters)
{
    FGuid SceneId;
    FAkUGCProjectDocument Document = MakeDocument(SceneId);
    const FGuid EntityId = FGuid::NewGuid();

    FAkUGCCommand Add = MakeCommand(EAkUGCCommandType::AddEntity, SceneId, EntityId);
    Add.Entity = MakeEntity(EntityId);

    FAkUGCCommand InvalidProperty = MakeCommand(EAkUGCCommandType::SetProperty, SceneId, EntityId);
    InvalidProperty.ComponentTypeId = TEXT("missing.component");
    InvalidProperty.PropertyId = TEXT("value");

    const FAkUGCCommandTransaction Transaction = MakeTransaction(TEXT("Atomic failure"), {Add, InvalidProperty});
    const FAkUGCCommandExecutionResult Result = FAkUGCCommandExecutor::Apply(Document, Transaction);
    TestFalse(TEXT("Transaction fails"), Result.bSucceeded);
    TestEqual(TEXT("Failed transaction does not add entity"), Document.Scenes[0].Entities.Num(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCCommandJsonTest,
    "AkUGC.Core.Command.JsonRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCCommandJsonTest::RunTest(const FString& Parameters)
{
    const FGuid SceneId = FGuid::NewGuid();
    const FGuid EntityId = FGuid::NewGuid();
    FAkUGCCommand Command = MakeCommand(EAkUGCCommandType::SetTransform, SceneId, EntityId);
    Command.Transform = FTransform(FVector(10.0, 20.0, 30.0));
    const FAkUGCCommandTransaction Source = MakeTransaction(TEXT("Move entity"), {Command});

    FString Json;
    FString Error;
    TestTrue(TEXT("Command transaction serializes"), FAkUGCCommandJson::Serialize(Source, Json, &Error));

    FAkUGCCommandTransaction Restored;
    TestTrue(TEXT("Command transaction deserializes"), FAkUGCCommandJson::Deserialize(Json, Restored, &Error));
    TestEqual(TEXT("Transaction ID round-trips"), Restored.TransactionId, Source.TransactionId);
    TestEqual(TEXT("Command type round-trips"), Restored.Commands[0].Type, EAkUGCCommandType::SetTransform);
    TestEqual(TEXT("Transform round-trips"), Restored.Commands[0].Transform.GetLocation(), FVector(10.0, 20.0, 30.0));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCCommandSetParentTest,
    "AkUGC.Core.Command.SetParentUndoRedo",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCCommandSetParentTest::RunTest(const FString& Parameters)
{
    FGuid SceneId;
    FAkUGCProjectDocument Document = MakeDocument(SceneId);
    const FGuid ParentId = FGuid::NewGuid();
    const FGuid ChildId = FGuid::NewGuid();
    Document.Scenes[0].Entities = {MakeEntity(ParentId), MakeEntity(ChildId)};

    FAkUGCCommand SetParent = MakeCommand(EAkUGCCommandType::SetParent, SceneId, ChildId);
    SetParent.ParentEntityId = ParentId;
    FAkUGCCommandHistory History;
    TestTrue(
        TEXT("SetParent transaction succeeds"),
        History.Execute(Document, MakeTransaction(TEXT("Parent child"), {SetParent})).bSucceeded);
    TestEqual(TEXT("Child references parent"), Document.Scenes[0].Entities[1].ParentEntityId, ParentId);

    TestTrue(TEXT("Undo SetParent succeeds"), History.Undo(Document).bSucceeded);
    TestFalse(TEXT("Undo restores child to scene root"), Document.Scenes[0].Entities[1].ParentEntityId.IsValid());
    TestTrue(TEXT("Redo SetParent succeeds"), History.Redo(Document).bSucceeded);
    TestEqual(TEXT("Redo restores parent"), Document.Scenes[0].Entities[1].ParentEntityId, ParentId);

    FAkUGCCommand CreateCycle = MakeCommand(EAkUGCCommandType::SetParent, SceneId, ParentId);
    CreateCycle.ParentEntityId = ChildId;
    TestFalse(
        TEXT("SetParent rejects hierarchy cycle"),
        History.Execute(Document, MakeTransaction(TEXT("Create cycle"), {CreateCycle})).bSucceeded);
    TestFalse(TEXT("Rejected cycle leaves parent at scene root"), Document.Scenes[0].Entities[0].ParentEntityId.IsValid());

    FAkUGCCommand MissingParent = MakeCommand(EAkUGCCommandType::SetParent, SceneId, ChildId);
    MissingParent.ParentEntityId = FGuid::NewGuid();
    TestFalse(
        TEXT("SetParent rejects missing parent"),
        History.Execute(Document, MakeTransaction(TEXT("Missing parent"), {MissingParent})).bSucceeded);
    TestEqual(TEXT("Rejected parent keeps document unchanged"), Document.Scenes[0].Entities[1].ParentEntityId, ParentId);

    FString Json;
    FString Error;
    const FAkUGCCommandTransaction Source = MakeTransaction(TEXT("Serialize parent"), {SetParent});
    TestTrue(TEXT("SetParent transaction serializes"), FAkUGCCommandJson::Serialize(Source, Json, &Error));
    FAkUGCCommandTransaction Restored;
    TestTrue(TEXT("SetParent transaction deserializes"), FAkUGCCommandJson::Deserialize(Json, Restored, &Error));
    TestEqual(TEXT("SetParent type round-trips"), Restored.Commands[0].Type, EAkUGCCommandType::SetParent);
    TestEqual(TEXT("Parent entity ID round-trips"), Restored.Commands[0].ParentEntityId, ParentId);
    return true;
}

#endif

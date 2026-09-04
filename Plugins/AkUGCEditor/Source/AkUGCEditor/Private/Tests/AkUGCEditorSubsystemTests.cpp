#include "Misc/AutomationTest.h"

#include "Editor.h"
#include "Misc/App.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Subsystem/AkUGCEditorSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCEditorSubsystemWorkflowTest,
    "AkUGC.Editor.CreatorStudio.MinimalWorkflow",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCEditorSubsystemWorkflowTest::RunTest(const FString& Parameters)
{
    UAkUGCEditorSubsystem* Subsystem = GEditor
        ? GEditor->GetEditorSubsystem<UAkUGCEditorSubsystem>()
        : nullptr;
    TestNotNull(TEXT("Creator Studio subsystem exists"), Subsystem);
    if (!Subsystem)
    {
        return false;
    }

    FString Error;
    TestTrue(TEXT("Tower defense project is created"), Subsystem->NewTowerDefenseProject(&Error));

    FGuid EntityId;
    const FAkUGCCommandExecutionResult PlaceResult = Subsystem->PlacePrefab(
        TEXT("official.gameplay.base"),
        FTransform(FVector(100.0, 0.0, 0.0)),
        EntityId);
    TestTrue(TEXT("Official prefab is placed"), PlaceResult.bSucceeded);
    TestEqual(TEXT("Document contains placed entity"), Subsystem->GetDocument().Scenes[0].Entities.Num(), 1);
    if (!IsRunningCommandlet() && !FApp::IsUnattended())
    {
        TestEqual(TEXT("Placed entity is selected in the editor"), Subsystem->GetSelectedEntityId(), EntityId);
    }

    FGuid DuplicateId;
    TestTrue(TEXT("Entity duplicates"), Subsystem->DuplicateEntity(EntityId, DuplicateId).bSucceeded);
    if (!IsRunningCommandlet() && !FApp::IsUnattended())
    {
        TestEqual(TEXT("Duplicate is selected"), Subsystem->GetSelectedEntityId(), DuplicateId);
    }
    TestEqual(TEXT("Document contains duplicate"), Subsystem->GetDocument().Scenes[0].Entities.Num(), 2);
    TestTrue(TEXT("Duplicate deletes"), Subsystem->DeleteEntity(DuplicateId).bSucceeded);
    TestEqual(TEXT("Delete removes duplicate"), Subsystem->GetDocument().Scenes[0].Entities.Num(), 1);
    TestTrue(TEXT("Undo delete restores duplicate"), Subsystem->Undo().bSucceeded);
    TestEqual(TEXT("Undo delete restores document entity"), Subsystem->GetDocument().Scenes[0].Entities.Num(), 2);
    TestTrue(TEXT("Undo duplicate removes duplicate"), Subsystem->Undo().bSucceeded);
    TestEqual(TEXT("Undo duplicate restores one entity"), Subsystem->GetDocument().Scenes[0].Entities.Num(), 1);

    TestTrue(TEXT("Creator undo succeeds"), Subsystem->Undo().bSucceeded);
    TestEqual(TEXT("Undo removes document entity"), Subsystem->GetDocument().Scenes[0].Entities.Num(), 0);
    TestTrue(TEXT("Creator redo succeeds"), Subsystem->Redo().bSucceeded);
    TestEqual(TEXT("Redo restores document entity"), Subsystem->GetDocument().Scenes[0].Entities.Num(), 1);

    const FString TestPath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation/CreatorStudioWorkflow.json"));
    TestTrue(TEXT("Creator project saves"), Subsystem->SaveProject(TestPath, &Error));
    Subsystem->CloseProject();
    TestFalse(TEXT("Close clears active project"), Subsystem->HasOpenProject());
    TestTrue(TEXT("Creator project loads"), Subsystem->LoadProject(TestPath, &Error));
    TestEqual(TEXT("Loaded project restores entity"), Subsystem->GetDocument().Scenes[0].Entities.Num(), 1);

    Subsystem->CloseProject();
    IFileManager::Get().Delete(*TestPath, false, true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCEditorSchemaPropertyTest,
    "AkUGC.Editor.CreatorStudio.SchemaPropertyEditing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCEditorSchemaPropertyTest::RunTest(const FString& Parameters)
{
    UAkUGCEditorSubsystem* Subsystem = GEditor
        ? GEditor->GetEditorSubsystem<UAkUGCEditorSubsystem>()
        : nullptr;
    TestNotNull(TEXT("Creator Studio subsystem exists"), Subsystem);
    if (!Subsystem)
    {
        return false;
    }

    FString Error;
    TestTrue(TEXT("Tower defense project is created"), Subsystem->NewTowerDefenseProject(&Error));

    FGuid EntityId;
    TestTrue(
        TEXT("Base prefab is placed"),
        Subsystem->PlacePrefab(TEXT("official.gameplay.base"), FTransform::Identity, EntityId).bSucceeded);
    const uint64 RevisionBeforeEdit = Subsystem->GetDocumentRevision();

    FAkUGCValue MaxHealth;
    MaxHealth.Type = EAkUGCValueType::Number;
    MaxHealth.NumberValue = 2500.0;
    TestTrue(
        TEXT("Schema property edit succeeds"),
        Subsystem->SetEntityProperty(EntityId, TEXT("core.health"), TEXT("maxHealth"), MaxHealth).bSucceeded);
    TestTrue(TEXT("Property edit advances document revision"), Subsystem->GetDocumentRevision() > RevisionBeforeEdit);

    const FAkUGCEntityRecord* Entity = Subsystem->FindEntity(EntityId);
    TestNotNull(TEXT("Edited entity exists"), Entity);
    const FAkUGCComponentRecord* Health = Entity
        ? Entity->Components.FindByPredicate([](const FAkUGCComponentRecord& Component)
        {
            return Component.TypeId == TEXT("core.health");
        })
        : nullptr;
    TestNotNull(TEXT("Health component exists"), Health);
    const FAkUGCValue* StoredMaxHealth = Health ? Health->Properties.Find(TEXT("maxHealth")) : nullptr;
    TestNotNull(TEXT("Edited maxHealth exists"), StoredMaxHealth);
    if (StoredMaxHealth)
    {
        TestEqual(TEXT("Edited maxHealth is stored"), StoredMaxHealth->NumberValue, 2500.0);
    }

    const uint64 RevisionBeforeInvalidEdit = Subsystem->GetDocumentRevision();
    MaxHealth.NumberValue = 200000.0;
    TestFalse(
        TEXT("Out-of-range schema property is rejected"),
        Subsystem->SetEntityProperty(EntityId, TEXT("core.health"), TEXT("maxHealth"), MaxHealth).bSucceeded);
    MaxHealth.NumberValue = TNumericLimits<double>::Max() * 2.0;
    TestFalse(
        TEXT("Non-finite schema property is rejected"),
        Subsystem->SetEntityProperty(EntityId, TEXT("core.health"), TEXT("maxHealth"), MaxHealth).bSucceeded);
    TestEqual(
        TEXT("Rejected property edits do not advance document revision"),
        Subsystem->GetDocumentRevision(),
        RevisionBeforeInvalidEdit);

    TestTrue(TEXT("Undo property edit succeeds"), Subsystem->Undo().bSucceeded);
    Entity = Subsystem->FindEntity(EntityId);
    Health = Entity
        ? Entity->Components.FindByPredicate([](const FAkUGCComponentRecord& Component)
        {
            return Component.TypeId == TEXT("core.health");
        })
        : nullptr;
    StoredMaxHealth = Health ? Health->Properties.Find(TEXT("maxHealth")) : nullptr;
    TestNotNull(TEXT("Undo keeps maxHealth property"), StoredMaxHealth);
    if (StoredMaxHealth)
    {
        TestEqual(TEXT("Undo restores default maxHealth"), StoredMaxHealth->NumberValue, 1000.0);
    }

    TestTrue(TEXT("Redo property edit succeeds"), Subsystem->Redo().bSucceeded);
    Entity = Subsystem->FindEntity(EntityId);
    Health = Entity
        ? Entity->Components.FindByPredicate([](const FAkUGCComponentRecord& Component)
        {
            return Component.TypeId == TEXT("core.health");
        })
        : nullptr;
    StoredMaxHealth = Health ? Health->Properties.Find(TEXT("maxHealth")) : nullptr;
    TestNotNull(TEXT("Redo keeps maxHealth property"), StoredMaxHealth);
    if (StoredMaxHealth)
    {
        TestEqual(TEXT("Redo restores edited maxHealth"), StoredMaxHealth->NumberValue, 2500.0);
    }

    Subsystem->CloseProject();
    return true;
}

#endif

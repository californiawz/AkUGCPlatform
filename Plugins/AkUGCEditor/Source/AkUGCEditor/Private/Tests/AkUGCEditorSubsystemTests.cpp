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

#endif

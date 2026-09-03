#include "Misc/AutomationTest.h"

#include "Editor.h"
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

#include "Session/AkUGCDocumentRuntimeSession.h"

#include "Document/AkUGCDocument.h"
#include "Prefab/AkUGCPrefabRegistry.h"

FAkUGCDocumentRuntimeSession::FAkUGCDocumentRuntimeSession(
    FAkUGCSceneRuntime& InRuntime,
    const FAkUGCPrefabRegistry& InRegistry,
    FGuid InSceneId,
    int32 MaxHistoryEntries)
    : FAkUGCDocumentRuntimeSession(
        InRuntime,
        InRegistry,
        InSceneId,
        EAkUGCRuntimeSessionMode::Edit,
        MaxHistoryEntries)
{
}

FAkUGCDocumentRuntimeSession::FAkUGCDocumentRuntimeSession(
    FAkUGCSceneRuntime& InRuntime,
    const FAkUGCPrefabRegistry& InRegistry,
    FGuid InSceneId,
    EAkUGCRuntimeSessionMode InMode,
    int32 MaxHistoryEntries)
    : Runtime(InRuntime)
    , Registry(InRegistry)
    , SceneId(InSceneId)
    , Mode(InMode)
    , History(MaxHistoryEntries)
{
}

FAkUGCDocumentRuntimeSession::~FAkUGCDocumentRuntimeSession()
{
    Runtime.CancelLogicExecution(ExecutionOwnerId);
    *LifetimeToken = false;
}

FAkUGCCommandExecutionResult FAkUGCDocumentRuntimeSession::Initialize(FAkUGCProjectDocument& Document)
{
    const FAkUGCSceneDocument* Scene = FindScene(Document);
    if (!Scene)
    {
        return FAkUGCCommandExecutionResult::Failure(TEXT("session.sceneId"), TEXT("Session scene does not exist in the document."));
    }

    if (Runtime.LogicExecutionOwnerId.IsValid()
        && Runtime.LogicExecutionOwnerId != ExecutionOwnerId)
    {
        return FAkUGCCommandExecutionResult::Failure(
            TEXT("session.executionOwnerId"),
            TEXT("Scene runtime is already bound to another Logic session."));
    }

    const bool bRunGameStart = Mode == EAkUGCRuntimeSessionMode::Preview
        || Mode == EAkUGCRuntimeSessionMode::PlayAuthority;
    const bool bRequiresTowerDefenseGameplay = bRunGameStart
        && Document.Manifest.TemplateId == TEXT("official.tower_defense");
    if (bRequiresTowerDefenseGameplay)
    {
        const FAkUGCTowerDefensePathBuildResult PathResult = FAkUGCTowerDefensePathBuilder::Build(*Scene, true);
        if (!PathResult.bSucceeded)
        {
            return FAkUGCCommandExecutionResult::Failure(
                TEXT("runtime.path.") + PathResult.ErrorPath,
                PathResult.ErrorMessage);
        }
        FString GameplayError;
        if (!Runtime.ValidateTowerDefenseGameplay(*Scene, Registry, &GameplayError))
        {
            return FAkUGCCommandExecutionResult::Failure(
                TEXT("runtime.towerDefense"),
                MoveTemp(GameplayError));
        }
    }

    FString Error;
    if (!Runtime.LoadScene(*Scene, Registry, &Error))
    {
        return FAkUGCCommandExecutionResult::Failure(TEXT("runtime.initialize"), MoveTemp(Error));
    }
    if (bRequiresTowerDefenseGameplay
        && !Runtime.InitializeTowerDefenseGameplay(*Scene, Registry, &Error))
    {
        Runtime.Unload();
        return FAkUGCCommandExecutionResult::Failure(TEXT("runtime.towerDefense"), MoveTemp(Error));
    }
    if (bRunGameStart
        && !Runtime.RunGameStartLogic(
            *Scene,
            Registry,
            LifetimeToken,
            ExecutionOwnerId,
            Mode == EAkUGCRuntimeSessionMode::PlayAuthority,
            &Error))
    {
        Runtime.Unload();
        return FAkUGCCommandExecutionResult::Failure(TEXT("runtime.logic.gameStart"), MoveTemp(Error));
    }

    History.Reset();
    return FAkUGCCommandExecutionResult::Success();
}

FAkUGCCommandExecutionResult FAkUGCDocumentRuntimeSession::Execute(
    FAkUGCProjectDocument& Document,
    const FAkUGCCommandTransaction& Transaction)
{
    if (Mode != EAkUGCRuntimeSessionMode::Edit)
    {
        return FAkUGCCommandExecutionResult::Failure(
            TEXT("session.mode"),
            TEXT("Document commands are only available in Edit sessions."));
    }
    return History.Execute(
        Document,
        Transaction,
        [this](const FAkUGCProjectDocument& Before, const FAkUGCProjectDocument& After, const FAkUGCCommandTransaction& Applied)
        {
            return Project(Before, After, Applied);
        });
}

FAkUGCCommandExecutionResult FAkUGCDocumentRuntimeSession::Undo(FAkUGCProjectDocument& Document)
{
    if (Mode != EAkUGCRuntimeSessionMode::Edit)
    {
        return FAkUGCCommandExecutionResult::Failure(
            TEXT("session.mode"),
            TEXT("Undo is only available in Edit sessions."));
    }
    return History.Undo(
        Document,
        [this](const FAkUGCProjectDocument& Before, const FAkUGCProjectDocument& After, const FAkUGCCommandTransaction& Applied)
        {
            return Project(Before, After, Applied);
        });
}

FAkUGCCommandExecutionResult FAkUGCDocumentRuntimeSession::Redo(FAkUGCProjectDocument& Document)
{
    if (Mode != EAkUGCRuntimeSessionMode::Edit)
    {
        return FAkUGCCommandExecutionResult::Failure(
            TEXT("session.mode"),
            TEXT("Redo is only available in Edit sessions."));
    }
    return History.Redo(
        Document,
        [this](const FAkUGCProjectDocument& Before, const FAkUGCProjectDocument& After, const FAkUGCCommandTransaction& Applied)
        {
            return Project(Before, After, Applied);
        });
}

bool FAkUGCDocumentRuntimeSession::CanUndo() const
{
    return Mode == EAkUGCRuntimeSessionMode::Edit && History.CanUndo();
}

bool FAkUGCDocumentRuntimeSession::CanRedo() const
{
    return Mode == EAkUGCRuntimeSessionMode::Edit && History.CanRedo();
}

void FAkUGCDocumentRuntimeSession::ResetHistory()
{
    History.Reset();
}

FAkUGCCommandExecutionResult FAkUGCDocumentRuntimeSession::Project(
    const FAkUGCProjectDocument& Before,
    const FAkUGCProjectDocument& After,
    const FAkUGCCommandTransaction& AppliedTransaction)
{
    const FAkUGCSceneDocument* AfterScene = FindScene(After);
    if (!AfterScene)
    {
        return FAkUGCCommandExecutionResult::Failure(TEXT("runtime.sceneId"), TEXT("Session scene was removed from the document."));
    }

    FString ProjectionError;
    if (Runtime.ApplyTransaction(AppliedTransaction, *AfterScene, Registry, &ProjectionError))
    {
        return FAkUGCCommandExecutionResult::Success();
    }

    FString RollbackError;
    const FAkUGCSceneDocument* BeforeScene = FindScene(Before);
    const bool bRollbackSucceeded = BeforeScene
        ? Runtime.LoadScene(*BeforeScene, Registry, &RollbackError)
        : (Runtime.Unload(), true);

    FString Message = FString::Printf(
        TEXT("Runtime projection failed for transaction '%s': %s"),
        *AppliedTransaction.Label,
        *ProjectionError);
    if (!bRollbackSucceeded)
    {
        Message += FString::Printf(TEXT("; runtime rollback also failed: %s"), *RollbackError);
    }
    return FAkUGCCommandExecutionResult::Failure(TEXT("runtime.projection"), MoveTemp(Message));
}

const FAkUGCSceneDocument* FAkUGCDocumentRuntimeSession::FindScene(const FAkUGCProjectDocument& Document) const
{
    return Document.Scenes.FindByPredicate([this](const FAkUGCSceneDocument& Scene)
    {
        return Scene.SceneId == SceneId;
    });
}

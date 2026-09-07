#include "Session/AkUGCDocumentRuntimeSession.h"

#include "Document/AkUGCDocument.h"
#include "Prefab/AkUGCPrefabRegistry.h"

FAkUGCDocumentRuntimeSession::FAkUGCDocumentRuntimeSession(
    FAkUGCSceneRuntime& InRuntime,
    const FAkUGCPrefabRegistry& InRegistry,
    FGuid InSceneId,
    int32 MaxHistoryEntries)
    : Runtime(InRuntime)
    , Registry(InRegistry)
    , SceneId(InSceneId)
    , History(MaxHistoryEntries)
{
}

FAkUGCDocumentRuntimeSession::~FAkUGCDocumentRuntimeSession()
{
    *LifetimeToken = false;
    Runtime.CancelLogicExecution();
}

FAkUGCCommandExecutionResult FAkUGCDocumentRuntimeSession::Initialize(FAkUGCProjectDocument& Document)
{
    const FAkUGCSceneDocument* Scene = FindScene(Document);
    if (!Scene)
    {
        return FAkUGCCommandExecutionResult::Failure(TEXT("session.sceneId"), TEXT("Session scene does not exist in the document."));
    }

    FString Error;
    if (!Runtime.LoadScene(*Scene, Registry, &Error))
    {
        return FAkUGCCommandExecutionResult::Failure(TEXT("runtime.initialize"), MoveTemp(Error));
    }
    if (!Runtime.RunGameStartLogic(*Scene, Registry, LifetimeToken, &Error))
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
    return History.Undo(
        Document,
        [this](const FAkUGCProjectDocument& Before, const FAkUGCProjectDocument& After, const FAkUGCCommandTransaction& Applied)
        {
            return Project(Before, After, Applied);
        });
}

FAkUGCCommandExecutionResult FAkUGCDocumentRuntimeSession::Redo(FAkUGCProjectDocument& Document)
{
    return History.Redo(
        Document,
        [this](const FAkUGCProjectDocument& Before, const FAkUGCProjectDocument& After, const FAkUGCCommandTransaction& Applied)
        {
            return Project(Before, After, Applied);
        });
}

bool FAkUGCDocumentRuntimeSession::CanUndo() const
{
    return History.CanUndo();
}

bool FAkUGCDocumentRuntimeSession::CanRedo() const
{
    return History.CanRedo();
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
    if (Runtime.ApplyTransaction(AppliedTransaction, Registry, &ProjectionError))
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

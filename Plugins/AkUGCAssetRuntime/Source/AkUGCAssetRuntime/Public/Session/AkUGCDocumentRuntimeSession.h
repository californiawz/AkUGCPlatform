#pragma once

#include "Command/AkUGCCommandHistory.h"
#include "Scene/AkUGCSceneRuntime.h"

class FAkUGCPrefabRegistry;

enum class EAkUGCRuntimeSessionMode : uint8
{
    Edit,
    Preview,
    PlayAuthority,
    PlayClient
};

class AKUGCASSETRUNTIME_API FAkUGCDocumentRuntimeSession
{
public:
    FAkUGCDocumentRuntimeSession(
        FAkUGCSceneRuntime& InRuntime,
        const FAkUGCPrefabRegistry& InRegistry,
        FGuid InSceneId,
        int32 MaxHistoryEntries = 100);

    FAkUGCDocumentRuntimeSession(
        FAkUGCSceneRuntime& InRuntime,
        const FAkUGCPrefabRegistry& InRegistry,
        FGuid InSceneId,
        EAkUGCRuntimeSessionMode InMode,
        int32 MaxHistoryEntries = 100);
    ~FAkUGCDocumentRuntimeSession();

    FAkUGCDocumentRuntimeSession(const FAkUGCDocumentRuntimeSession&) = delete;
    FAkUGCDocumentRuntimeSession& operator=(const FAkUGCDocumentRuntimeSession&) = delete;
    FAkUGCDocumentRuntimeSession(FAkUGCDocumentRuntimeSession&&) = delete;
    FAkUGCDocumentRuntimeSession& operator=(FAkUGCDocumentRuntimeSession&&) = delete;

    FAkUGCCommandExecutionResult Initialize(FAkUGCProjectDocument& Document);

    FAkUGCCommandExecutionResult Execute(
        FAkUGCProjectDocument& Document,
        const FAkUGCCommandTransaction& Transaction);

    FAkUGCCommandExecutionResult Undo(FAkUGCProjectDocument& Document);
    FAkUGCCommandExecutionResult Redo(FAkUGCProjectDocument& Document);

    bool CanUndo() const;
    bool CanRedo() const;
    void ResetHistory();

private:
    FAkUGCCommandExecutionResult Project(
        const FAkUGCProjectDocument& Before,
        const FAkUGCProjectDocument& After,
        const FAkUGCCommandTransaction& AppliedTransaction);

    const FAkUGCSceneDocument* FindScene(const FAkUGCProjectDocument& Document) const;

    FAkUGCSceneRuntime& Runtime;
    const FAkUGCPrefabRegistry& Registry;
    FGuid SceneId;
    FGuid ExecutionOwnerId = FGuid::NewGuid();
    EAkUGCRuntimeSessionMode Mode = EAkUGCRuntimeSessionMode::Edit;
    TSharedRef<bool, ESPMode::ThreadSafe> LifetimeToken = MakeShared<bool, ESPMode::ThreadSafe>(true);
    FAkUGCCommandHistory History;
};

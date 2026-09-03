#pragma once

#include "Command/AkUGCCommandHistory.h"
#include "Scene/AkUGCSceneRuntime.h"

class FAkUGCPrefabRegistry;

class AKUGCASSETRUNTIME_API FAkUGCDocumentRuntimeSession
{
public:
    FAkUGCDocumentRuntimeSession(
        FAkUGCSceneRuntime& InRuntime,
        const FAkUGCPrefabRegistry& InRegistry,
        FGuid InSceneId,
        int32 MaxHistoryEntries = 100);

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
    FAkUGCCommandHistory History;
};

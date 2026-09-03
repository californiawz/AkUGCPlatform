#pragma once

#include "Widgets/SCompoundWidget.h"

class UAkUGCEditorSubsystem;

class SAkUGCCreatorPanel final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SAkUGCCreatorPanel) {}
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs, UAkUGCEditorSubsystem* InSubsystem);

private:
    FReply NewProject();
    FReply SaveProject();
    FReply LoadProject();
    FReply Undo();
    FReply Redo();
    FReply PlacePrefab(FName PrefabId);
    FText GetStatusText() const;

    TWeakObjectPtr<UAkUGCEditorSubsystem> Subsystem;
    FString Status;
    int32 PlacementIndex = 0;
};

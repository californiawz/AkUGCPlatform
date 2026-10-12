#pragma once

#include "Widgets/SCompoundWidget.h"
#include "Pack/AkUGCWorkLibrary.h"

class SVerticalBox;
class UAkUGCEditorSubsystem;

class SAkUGCWorkLibraryPanel final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SAkUGCWorkLibraryPanel) {}
    SLATE_END_ARGS()
    void Construct(const FArguments& InArgs, UAkUGCEditorSubsystem* InSubsystem);
private:
    friend struct FAkUGCWorkLibraryTestAccess;
    FReply RequestDelete(const FAkUGCWorkEntry& Entry);
    FReply ConfirmDelete();
    FReply CancelDelete();
    FGuid PendingDeleteId;
    FString PendingDeleteName;
    FReply Publish();
    FReply Import();
    FReply Refresh();
    TWeakObjectPtr<UAkUGCEditorSubsystem> Subsystem;
    TSharedPtr<SVerticalBox> Rows;
    FString SharePath;
    FString TrustedKey;
    FString Status;
    bool bConfirmedTrust = false;
};

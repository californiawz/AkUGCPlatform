#pragma once

#include "Widgets/SCompoundWidget.h"
#include "Pack/AkUGCWorkLibrary.h"

class SVerticalBox;
class SEditableTextBox;
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
    FReply BackupIdentity();
    FReply RestoreIdentity();
    FReply RotateIdentity();
    FReply Refresh();
    bool ReadRecoveryPassphrase(
        bool bRequireConfirmation,
        TArray<uint8>& OutPassphrase,
        FString& OutError) const;
    void ClearRecoveryPassphrase();
    TWeakObjectPtr<UAkUGCEditorSubsystem> Subsystem;
    TSharedPtr<SVerticalBox> Rows;
    TSharedPtr<SEditableTextBox> RecoveryPassphraseBox;
    TSharedPtr<SEditableTextBox> RecoveryPassphraseConfirmBox;
    FString SharePath;
    FString TrustedKey;
    FString RecoveryPath;
    FString Status;
    bool bConfirmedTrust = false;
    bool bConfirmedIdentityRotation = false;
};

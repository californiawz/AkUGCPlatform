#pragma once

#include "Widgets/SCompoundWidget.h"
#include "Pack/AkUGCWorkLibrary.h"
#include "Registry/AkUGCReleaseRegistryClient.h"

class SVerticalBox;
class SEditableTextBox;
class UAkUGCEditorSubsystem;

class SAkUGCWorkLibraryPanel final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SAkUGCWorkLibraryPanel) {}
    SLATE_END_ARGS()
    void Construct(const FArguments& InArgs, UAkUGCEditorSubsystem* InSubsystem);
    virtual ~SAkUGCWorkLibraryPanel() override;
    virtual void Tick(
        const FGeometry& Geometry,
        double CurrentTime,
        float DeltaTime) override;
private:
    friend struct FAkUGCWorkLibraryTestAccess;
#if WITH_DEV_AUTOMATION_TESTS
    friend struct FAkUGCReleaseRegistryPanelTestAccess;
#endif
    FReply RequestDelete(const FAkUGCWorkEntry& Entry);
    FReply ConfirmDelete();
    FReply CancelDelete();
    FGuid PendingDeleteId;
    FString PendingDeleteName;
    FReply Publish();
    FReply SubmitToRegistry();
    FReply RefreshRegistryStatus();
    FReply PublishToRegistry();
    FReply CancelRegistryRequest();
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
    void CompleteRegistryRequest(
        const FAkUGCEditorHttpRequestPtr& Request,
        FAkUGCEditorHttpResult Result);
    TWeakObjectPtr<UAkUGCEditorSubsystem> Subsystem;
    TSharedPtr<SVerticalBox> Rows;
    TSharedPtr<SEditableTextBox> RecoveryPassphraseBox;
    TSharedPtr<SEditableTextBox> RecoveryPassphraseConfirmBox;
    FString SharePath;
    FString TrustedKey;
    FString RecoveryPath;
    FString Status;
    FString RegistryBaseUrl;
    FString RegistryToken;
    FString RegistryStatus;
    FString RegistryReleaseStatus;
    FString LastPublishedSharePath;
    TOptional<FAkUGCWorkEntry> LastPublishedEntry;
    FAkUGCWorkEntry RegistryRequestEntry;
    FAkUGCEditorHttpRequestPtr PendingRegistryRequest;
    EAkUGCReleaseRegistryOperation PendingRegistryOperation =
        EAkUGCReleaseRegistryOperation::Submit;
    float RegistryRequestTimeoutSeconds = 120.0f;
    bool bConfirmedTrust = false;
    bool bConfirmedIdentityRotation = false;
};

#pragma once

#include "Widgets/SCompoundWidget.h"
#include "Pack/AkUGCWorkLibrary.h"
#include "Registry/AkUGCReleaseRegistryClient.h"

class SVerticalBox;
class SEditableTextBox;
class UAkUGCEditorSubsystem;

enum class EAkUGCRegistryPanelRequest : uint8
{
    None,
    EnsureProject,
    CreateProject,
    SubmitRelease,
    GetRelease,
    PublishRelease,
    RefreshProject,
    ListMembers,
    ListVersions,
    SetMember,
    RemoveMember
};

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
    FReply RefreshRegistryProject();
    FReply SetRegistryMember();
    FReply RemoveRegistryMember();
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
    bool StartRegistryRequest(
        EAkUGCRegistryPanelRequest RequestKind,
        const FAkUGCEditorHttpRequestPtr& Request,
        const FString& Error,
        const FString& PendingStatus);
    bool StartSubmitReleaseRequest();
    bool StartRefreshProjectRequest(const FString& PendingStatus);
    bool CurrentProjectIdentity(
        FGuid& OutProjectId,
        FString& OutDisplayName,
        FString& OutError) const;
    void CompleteReleaseRegistryRequest(
        const FAkUGCEditorHttpResult& Result);
    void CompleteProjectRegistryRequest(
        const FAkUGCEditorHttpResult& Result);
    void ClearRegistryProjectView();
    void UpdateRegistryProjectSummary();
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
    FGuid RegistryReleaseProjectId;
    FString RegistryProjectSummary;
    FString RegistryMembersSummary;
    FString RegistryVersionsSummary;
    FString RegistryMemberSubject;
    FString RegistryMemberRole = TEXT("viewer");
    FString LastPublishedSharePath;
    TOptional<FAkUGCWorkEntry> LastPublishedEntry;
    FAkUGCWorkEntry RegistryRequestEntry;
    FGuid RegistryRequestProjectId;
    FString RegistryRequestProjectName;
    FString RegistryRequestMemberSubject;
    FString RegistryRequestMemberRole;
    FAkUGCRegistryProjectView RegistryProject;
    FString RegistryProjectAccessRole;
    TArray<FAkUGCRegistryMemberView> RegistryMembers;
    TArray<FAkUGCReleaseRegistryView> RegistryVersions;
    FString RegistryVersionsNextCursor;
    FAkUGCEditorHttpRequestPtr PendingRegistryRequest;
    EAkUGCReleaseRegistryOperation PendingRegistryOperation =
        EAkUGCReleaseRegistryOperation::Submit;
    EAkUGCProjectRegistryOperation PendingProjectOperation =
        EAkUGCProjectRegistryOperation::GetProject;
    EAkUGCRegistryPanelRequest PendingRegistryRequestKind =
        EAkUGCRegistryPanelRequest::None;
    float RegistryRequestTimeoutSeconds = 120.0f;
    bool bRegistryChainCancelled = false;
    bool bConfirmedTrust = false;
    bool bConfirmedIdentityRotation = false;
};

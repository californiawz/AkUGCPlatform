#include "Widgets/SAkUGCWorkLibraryPanel.h"

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "Pack/AkUGCPublisherKeyStore.h"
#include "Subsystem/AkUGCEditorSubsystem.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
    void SecureResetString(FString& Value)
    {
        TArray<TCHAR>& Characters = Value.GetCharArray();
        if (!Characters.IsEmpty())
        {
            FMemory::Memzero(Characters.GetData(), Characters.Num() * sizeof(TCHAR));
        }
        Value.Reset();
    }
}

void SAkUGCWorkLibraryPanel::Construct(const FArguments& InArgs, UAkUGCEditorSubsystem* InSubsystem)
{
    Subsystem = InSubsystem;
    RegistryBaseUrl = TEXT("http://127.0.0.1:8088");
    RegistryStatus = TEXT("Registry token 只保留在当前面板内存中；本机 Alpha 只允许 loopback HTTP。请先完成一次本地签名发布。");
    RecoveryPath = FPaths::Combine(
        FPaths::ProjectSavedDir(),
        TEXT("UGC/Recovery"),
        FString::Printf(
            TEXT("publisher-%s-%s.akugcrecovery"),
            *FDateTime::UtcNow().ToString(TEXT("%Y%m%d-%H%M%S")),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8)));
    ChildSlot
    [
        SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight().Padding(0, 4)
        [SNew(STextBlock).Text(FText::FromString(TEXT("本地发布 / 分享 / 作品库")))]
        + SVerticalBox::Slot().AutoHeight()
        [SNew(STextBlock).Text(FText::FromString(TEXT("分享文件不含私钥。公钥须通过独立渠道核对；确认的是密钥，不是作者实名。发布私钥由当前 Windows 用户的 DPAPI 加密保存在 Saved/UGC/Publisher；该目录仍属敏感凭据，请勿分享或归档。"))).AutoWrapText(true)]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 4)
        [SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().Padding(2)
            [SNew(SButton).Text(FText::FromString(TEXT("一键签名发布当前项目")))
                .IsEnabled_Lambda([this] { return !PendingRegistryRequest.IsValid() && Subsystem.IsValid() && Subsystem->HasOpenProject(); })
                .OnClicked(this, &SAkUGCWorkLibraryPanel::Publish)]
            + SHorizontalBox::Slot().AutoWidth().Padding(2)
            [SNew(SButton).Text(FText::FromString(TEXT("刷新作品库"))).OnClicked(this, &SAkUGCWorkLibraryPanel::Refresh)]
            + SHorizontalBox::Slot().AutoWidth().Padding(2)
            [SNew(SButton).Text(FText::FromString(TEXT("打开分享目录"))).OnClicked_Lambda([]
                { FPlatformProcess::ExploreFolder(*FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UGC/Share")))); return FReply::Handled(); })]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 2)
        [SNew(STextBlock).Text(FText::FromString(TEXT("Release Registry（本机 Alpha）")))]
        + SVerticalBox::Slot().AutoHeight()
        [SNew(STextBlock).Text(FText::FromString(TEXT("上传会先确认当前云 Project，再提交已本地验签且 Project 绑定一致的签名包。服务端异步构建只深度校验并原样提升同一签名字节，不会重新 Cook、改写或代签；构建成功后才能审核发布。静态 token 不保存，服务端构建结果也不会替代 Player 本地验签。"))).AutoWrapText(true)]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 2)
        [SNew(SEditableTextBox)
            .IsEnabled_Lambda([this] { return !PendingRegistryRequest.IsValid(); })
            .HintText(FText::FromString(TEXT("Registry Base URL（仅 http://127.0.0.1 或 localhost）")))
            .Text_Lambda([this] { return FText::FromString(RegistryBaseUrl); })
            .OnTextChanged_Lambda([this](const FText& Text) { RegistryBaseUrl = Text.ToString(); })]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 2)
        [SNew(SEditableTextBox)
            .IsEnabled_Lambda([this] { return !PendingRegistryRequest.IsValid(); })
            .IsPassword(true)
            .HintText(FText::FromString(TEXT("Creator Bearer Token（仅面板内存，不保存）")))
            .OnTextChanged_Lambda([this](const FText& Text) { RegistryToken = Text.ToString(); })]
        + SVerticalBox::Slot().AutoHeight()
        [SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().Padding(2)
            [SNew(SButton).Text(FText::FromString(TEXT("登记项目并上传签名包")))
                .IsEnabled_Lambda([this]
                    {
                        return !PendingRegistryRequest.IsValid()
                            && Subsystem.IsValid()
                            && Subsystem->HasOpenProject()
                            && LastPublishedEntry.IsSet()
                            && !LastPublishedSharePath.IsEmpty()
                            && !RegistryToken.IsEmpty();
                    })
                .OnClicked(this, &SAkUGCWorkLibraryPanel::SubmitToRegistry)]
            + SHorizontalBox::Slot().AutoWidth().Padding(2)
            [SNew(SButton).Text(FText::FromString(TEXT("刷新 Registry 状态")))
                .IsEnabled_Lambda([this]
                    {
                        return !PendingRegistryRequest.IsValid()
                            && LastPublishedEntry.IsSet()
                            && !RegistryToken.IsEmpty();
                    })
                .OnClicked(this, &SAkUGCWorkLibraryPanel::RefreshRegistryStatus)]
            + SHorizontalBox::Slot().AutoWidth().Padding(2)
            [SNew(SButton).Text(FText::FromString(TEXT("重试失败构建")))
                .IsEnabled_Lambda([this]
                    {
                        return !PendingRegistryRequest.IsValid()
                            && LastPublishedEntry.IsSet()
                            && RegistryBuild.Status == TEXT("failed")
                            && RegistryBuild.bRetryable
                            && !RegistryToken.IsEmpty();
                    })
                .OnClicked(this, &SAkUGCWorkLibraryPanel::RetryRegistryBuild)]
            + SHorizontalBox::Slot().AutoWidth().Padding(2)
            [SNew(SButton).Text(FText::FromString(TEXT("发布已审核 Release")))
                .IsEnabled_Lambda([this]
                    {
                        return !PendingRegistryRequest.IsValid()
                            && LastPublishedEntry.IsSet()
                            && RegistryReleaseStatus == TEXT("approved")
                            && RegistryBuild.Status == TEXT("succeeded")
                            && RegistryBuild.InputArtifactSha256
                                == RegistryReleaseArtifactSha256
                            && RegistryReleaseProjectId.IsValid()
                            && Subsystem.IsValid()
                            && Subsystem->HasOpenProject()
                            && Subsystem->GetDocument().Manifest.ProjectId
                                == RegistryReleaseProjectId
                            && !RegistryToken.IsEmpty();
                    })
                .OnClicked(this, &SAkUGCWorkLibraryPanel::PublishToRegistry)]
            + SHorizontalBox::Slot().AutoWidth().Padding(2)
            [SNew(SButton).Text(FText::FromString(TEXT("刷新云项目")))
                .IsEnabled_Lambda([this]
                    {
                        return !PendingRegistryRequest.IsValid()
                            && Subsystem.IsValid()
                            && Subsystem->HasOpenProject()
                            && !RegistryToken.IsEmpty();
                    })
                .OnClicked(this, &SAkUGCWorkLibraryPanel::RefreshRegistryProject)]
            + SHorizontalBox::Slot().AutoWidth().Padding(2)
            [SNew(SButton).Text(FText::FromString(TEXT("取消 Registry 请求")))
                .IsEnabled_Lambda([this] { return PendingRegistryRequest.IsValid(); })
                .OnClicked(this, &SAkUGCWorkLibraryPanel::CancelRegistryRequest)]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 2)
        [SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().Padding(2)
            [SNew(SButton).Text(FText::FromString(TEXT("设为私人作品")))
                .IsEnabled_Lambda([this]
                    {
                        return !PendingRegistryRequest.IsValid()
                            && RegistryProjectAccessRole == TEXT("owner")
                            && RegistryProject.Visibility == TEXT("public")
                            && !RegistryToken.IsEmpty();
                    })
                .OnClicked_Lambda([this]
                    { return SetRegistryVisibility(TEXT("private")); })]
            + SHorizontalBox::Slot().AutoWidth().Padding(2)
            [SNew(SButton).Text(FText::FromString(TEXT("公开作品页")))
                .IsEnabled_Lambda([this]
                    {
                        return !PendingRegistryRequest.IsValid()
                            && RegistryProjectAccessRole == TEXT("owner")
                            && RegistryProject.Visibility == TEXT("private")
                            && !RegistryToken.IsEmpty();
                    })
                .OnClicked_Lambda([this]
                    { return SetRegistryVisibility(TEXT("public")); })]
            + SHorizontalBox::Slot().FillWidth(1.0f).Padding(6, 2)
            [SNew(STextBlock)
                .Text(FText::FromString(TEXT("公开仅允许项目外 Player 读取仍为 published 的最新版本；下载后仍须独立公钥本地验签。")))
                .AutoWrapText(true)]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 2)
        [SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1.0f).Padding(2)
            [SNew(SEditableTextBox)
                .IsEnabled_Lambda([this] { return !PendingRegistryRequest.IsValid(); })
                .HintText(FText::FromString(TEXT("协作者 subject")))
                .Text_Lambda([this] { return FText::FromString(RegistryMemberSubject); })
                .OnTextChanged_Lambda([this](const FText& Text)
                    { RegistryMemberSubject = Text.ToString(); })]
            + SHorizontalBox::Slot().AutoWidth().Padding(2)
            [SNew(SEditableTextBox)
                .IsEnabled_Lambda([this] { return !PendingRegistryRequest.IsValid(); })
                .MinDesiredWidth(100.0f)
                .HintText(FText::FromString(TEXT("editor / viewer")))
                .Text_Lambda([this] { return FText::FromString(RegistryMemberRole); })
                .OnTextChanged_Lambda([this](const FText& Text)
                    { RegistryMemberRole = Text.ToString(); })]
            + SHorizontalBox::Slot().AutoWidth().Padding(2)
            [SNew(SButton).Text(FText::FromString(TEXT("设置协作者")))
                .IsEnabled_Lambda([this]
                    {
                        return !PendingRegistryRequest.IsValid()
                            && RegistryProjectAccessRole == TEXT("owner")
                            && !RegistryMemberSubject.IsEmpty()
                            && !RegistryToken.IsEmpty();
                    })
                .OnClicked(this, &SAkUGCWorkLibraryPanel::SetRegistryMember)]
            + SHorizontalBox::Slot().AutoWidth().Padding(2)
            [SNew(SButton).Text(FText::FromString(TEXT("移除协作者")))
                .IsEnabled_Lambda([this]
                    {
                        return !PendingRegistryRequest.IsValid()
                            && RegistryProjectAccessRole == TEXT("owner")
                            && !RegistryMemberSubject.IsEmpty()
                            && !RegistryToken.IsEmpty();
                    })
                .OnClicked(this, &SAkUGCWorkLibraryPanel::RemoveRegistryMember)]
        ]
        + SVerticalBox::Slot().AutoHeight()
        [SNew(STextBlock).Text_Lambda([this] { return FText::FromString(RegistryStatus); }).AutoWrapText(true)]
        + SVerticalBox::Slot().AutoHeight()
        [SNew(STextBlock)
            .Text_Lambda([this]
                {
                    if (!RegistryBuild.ReleaseId.IsValid())
                    {
                        return FText::GetEmpty();
                    }
                    FString Summary = FString::Printf(
                        TEXT("可信构建：%s；尝试 %lld；修订 %lld"),
                        *RegistryBuild.Status,
                        static_cast<long long>(RegistryBuild.Attempt),
                        static_cast<long long>(RegistryBuild.Revision));
                    if (!RegistryBuild.ErrorCode.IsEmpty())
                    {
                        Summary += FString::Printf(
                            TEXT("；错误 %s%s"),
                            *RegistryBuild.ErrorCode,
                            RegistryBuild.bRetryable
                                ? TEXT("（可重试）")
                                : TEXT("（不可重试）"));
                    }
                    return FText::FromString(Summary);
                })
            .AutoWrapText(true)]
        + SVerticalBox::Slot().AutoHeight()
        [SNew(STextBlock).Text_Lambda([this] { return FText::FromString(RegistryProjectSummary); }).AutoWrapText(true)]
        + SVerticalBox::Slot().AutoHeight()
        [SNew(STextBlock).Text_Lambda([this] { return FText::FromString(RegistryMembersSummary); }).AutoWrapText(true)]
        + SVerticalBox::Slot().AutoHeight()
        [SNew(STextBlock).Text_Lambda([this] { return FText::FromString(RegistryVersionsSummary); }).AutoWrapText(true)]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 2)
        [SNew(STextBlock).Text(FText::FromString(TEXT("发布身份恢复与轮换")))]
        + SVerticalBox::Slot().AutoHeight()
        [SNew(STextBlock).Text(FText::FromString(TEXT("恢复包使用 scrypt + AES-256-GCM 加密，可跨 Windows 用户恢复，但仍是敏感凭据。请与密码分开保存，禁止提交仓库或归档到候选包。恢复只允许空目录或完全相同的身份；轮换会生成双签名历史并同时备份新身份。"))).AutoWrapText(true)]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 2)
        [SNew(SEditableTextBox)
            .HintText(FText::FromString(TEXT("身份恢复包完整路径（.akugcrecovery）")))
            .Text_Lambda([this] { return FText::FromString(RecoveryPath); })
            .OnTextChanged_Lambda([this](const FText& Text) { RecoveryPath = Text.ToString(); })]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 2)
        [SAssignNew(RecoveryPassphraseBox, SEditableTextBox)
            .IsPassword(true)
            .HintText(FText::FromString(TEXT("身份恢复密码（至少12字节，不保存）")))]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 2)
        [SAssignNew(RecoveryPassphraseConfirmBox, SEditableTextBox)
            .IsPassword(true)
            .HintText(FText::FromString(TEXT("再次输入恢复密码（导出/轮换必填）")))]
        + SVerticalBox::Slot().AutoHeight()
        [SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().Padding(2)
            [SNew(SButton).Text(FText::FromString(TEXT("导出加密身份备份")))
                .OnClicked(this, &SAkUGCWorkLibraryPanel::BackupIdentity)]
            + SHorizontalBox::Slot().AutoWidth().Padding(2)
            [SNew(SButton).Text(FText::FromString(TEXT("从加密备份恢复")))
                .OnClicked(this, &SAkUGCWorkLibraryPanel::RestoreIdentity)]
            + SHorizontalBox::Slot().AutoWidth().Padding(2)
            [SNew(SButton).Text(FText::FromString(TEXT("轮换身份并备份")))
                .IsEnabled_Lambda([this] { return bConfirmedIdentityRotation; })
                .OnClicked(this, &SAkUGCWorkLibraryPanel::RotateIdentity)]
        ]
        + SVerticalBox::Slot().AutoHeight()
        [SNew(SCheckBox)
            .IsChecked_Lambda([this] { return bConfirmedIdentityRotation ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
            .OnCheckStateChanged_Lambda([this](ECheckBoxState State) { bConfirmedIdentityRotation = State == ECheckBoxState::Checked; })
            [SNew(STextBlock).Text(FText::FromString(TEXT("我理解轮换后新作品使用新公钥；旧作品仍由历史公钥验签，旧恢复包不能回退当前身份"))).AutoWrapText(true)]]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 2)
        [SNew(SEditableTextBox).HintText(FText::FromString(TEXT("接收的 .akugc 分享文件完整路径")))
            .Text_Lambda([this] { return FText::FromString(SharePath); })
            .OnTextChanged_Lambda([this](const FText& Text) { SharePath = Text.ToString(); bConfirmedTrust = false; })]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 2)
        [SNew(SEditableTextBox).HintText(FText::FromString(TEXT("独立核对的 Ed25519 公钥（64位hex）")))
            .OnTextChanged_Lambda([this](const FText& Text) { TrustedKey = Text.ToString(); bConfirmedTrust = false; })]
        + SVerticalBox::Slot().AutoHeight()
        [SNew(SCheckBox).IsChecked_Lambda([this] { return bConfirmedTrust ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
            .OnCheckStateChanged_Lambda([this](ECheckBoxState State) { bConfirmedTrust = State == ECheckBoxState::Checked; })
            [SNew(STextBlock).Text(FText::FromString(TEXT("我已通过独立渠道核对该公钥，并信任其签发的本次作品"))).AutoWrapText(true)]]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 4)
        [SNew(SButton).Text(FText::FromString(TEXT("验签并导入作品库")))
            .IsEnabled_Lambda([this] { return bConfirmedTrust && !SharePath.IsEmpty() && !TrustedKey.IsEmpty(); })
            .OnClicked(this, &SAkUGCWorkLibraryPanel::Import)]
        + SVerticalBox::Slot().AutoHeight()
        [SNew(STextBlock).Text_Lambda([this] { return FText::FromString(Status); }).AutoWrapText(true)]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 4)
        [SNew(SBorder).Visibility_Lambda([this] { return PendingDeleteId.IsValid() ? EVisibility::Visible : EVisibility::Collapsed; })
            [SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight()
                [SNew(STextBlock).Text_Lambda([this] { return FText::FromString(TEXT("确认删除本地记录：") + PendingDeleteName + TEXT("\n")
                    + PendingDeleteId.ToString(EGuidFormats::Digits) + TEXT("\n不会删除分享文件或编辑项目。")); }).AutoWrapText(true)]
                + SVerticalBox::Slot().AutoHeight()
                [SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().Padding(2)
                    [SNew(SButton).Text(FText::FromString(TEXT("取消删除"))).OnClicked(this, &SAkUGCWorkLibraryPanel::CancelDelete)]
                    + SHorizontalBox::Slot().AutoWidth().Padding(2)
                    [SNew(SButton).Text(FText::FromString(TEXT("确认删除记录"))).OnClicked(this, &SAkUGCWorkLibraryPanel::ConfirmDelete)]
                ]
            ]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 4)
        [SAssignNew(Rows, SVerticalBox).IsEnabled_Lambda([this] { return !PendingDeleteId.IsValid(); })]
    ];
    Refresh();
}

SAkUGCWorkLibraryPanel::~SAkUGCWorkLibraryPanel()
{
    if (PendingRegistryRequest.IsValid())
    {
        PendingRegistryRequest->Cancel();
    }
    SecureResetString(RegistryToken);
}

void SAkUGCWorkLibraryPanel::Tick(
    const FGeometry& Geometry,
    double CurrentTime,
    float DeltaTime)
{
    SCompoundWidget::Tick(Geometry, CurrentTime, DeltaTime);
    if (PendingRegistryRequest.IsValid() && PendingRegistryRequest->IsDone())
    {
        const FAkUGCEditorHttpRequestPtr Completed = PendingRegistryRequest;
        CompleteRegistryRequest(Completed, Completed->TakeResult());
    }
}

FReply SAkUGCWorkLibraryPanel::Publish()
{
    FString Error, Path;
    FAkUGCWorkEntry Entry;
    if (Subsystem.IsValid() && Subsystem->HasOpenProject()
        && FAkUGCWorkLibrary(FAkUGCWorkLibrary::DefaultDirectory()).PublishWithPublisherIdentity(
            Subsystem->GetDocument(), FAkUGCPublisherKeyStore::DefaultDirectory(),
            FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UGC/Share")), Entry, Path, &Error))
    {
        SharePath = Path;
        LastPublishedEntry = Entry;
        LastPublishedSharePath = Path;
        RegistryReleaseStatus.Reset();
        RegistryReleaseArtifactSha256.Reset();
        RegistryReleaseArtifactSize = 0;
        RegistryReleaseProjectId.Invalidate();
        RegistryBuild = {};
        RegistryRequestArtifactSha256.Reset();
        RegistryRequestArtifactSize = 0;
        RegistryStatus = TEXT("本地签名包已就绪；可显式上传到 Registry。");
        bConfirmedTrust = false;
        Refresh();
        Status = FString::Printf(TEXT("发布完成：%s\n分享路径：%s\n独立核对公钥：%s\n已发布的是当前项目快照，不含尚未应用的参数草稿。"),
            *Entry.ReleaseId.ToString(EGuidFormats::Digits), *Path, *Entry.TrustedPublicKey);
    }
    else { Status = Error.IsEmpty() ? TEXT("请先打开项目。") : Error; }
    return FReply::Handled();
}

FReply SAkUGCWorkLibraryPanel::SubmitToRegistry()
{
    if (PendingRegistryRequest.IsValid() || !LastPublishedEntry.IsSet())
    {
        return FReply::Handled();
    }
    FString Error;
    FGuid ProjectId;
    FString ProjectName;
    if (!CurrentProjectIdentity(ProjectId, ProjectName, Error))
    {
        RegistryStatus = Error;
        return FReply::Handled();
    }
    if (RegistryProject.ProjectId.IsValid()
        && RegistryProject.ProjectId != ProjectId)
    {
        ClearRegistryProjectView();
    }
    const FAkUGCWorkEntry Entry = LastPublishedEntry.GetValue();
    if (!FAkUGCReleaseRegistryClient::ValidateSubmissionArtifact(
            Entry,
            LastPublishedSharePath,
            ProjectId,
            Error,
            &RegistryRequestArtifactSha256,
            &RegistryRequestArtifactSize))
    {
        RegistryStatus = Error;
        return FReply::Handled();
    }
    const FAkUGCEditorHttpRequestPtr Request =
        FAkUGCReleaseRegistryClient::StartGetProject(
            RegistryBaseUrl,
            RegistryToken,
            ProjectId,
            RegistryRequestTimeoutSeconds,
            Error);
    RegistryRequestEntry = Entry;
    RegistryRequestProjectId = ProjectId;
    RegistryRequestProjectName = ProjectName;
    RegistryBuild = {};
    PendingProjectOperation = EAkUGCProjectRegistryOperation::GetProject;
    bRegistryChainCancelled = false;
    StartRegistryRequest(
        EAkUGCRegistryPanelRequest::EnsureProject,
        Request,
        Error,
        TEXT("正在确认云 Project；不存在时会先登记，再上传本地签名包。"));
    return FReply::Handled();
}

FReply SAkUGCWorkLibraryPanel::RefreshRegistryStatus()
{
    if (PendingRegistryRequest.IsValid() || !LastPublishedEntry.IsSet())
    {
        return FReply::Handled();
    }
    FString Error;
    FGuid ProjectId;
    FString ProjectName;
    if (!CurrentProjectIdentity(ProjectId, ProjectName, Error))
    {
        RegistryStatus = Error;
        return FReply::Handled();
    }
    const FAkUGCWorkEntry Entry = LastPublishedEntry.GetValue();
    if (!FAkUGCReleaseRegistryClient::ValidateSubmissionArtifact(
            Entry,
            LastPublishedSharePath,
            ProjectId,
            Error,
            &RegistryRequestArtifactSha256,
            &RegistryRequestArtifactSize))
    {
        RegistryStatus = Error;
        return FReply::Handled();
    }
    const FAkUGCEditorHttpRequestPtr Request =
        FAkUGCReleaseRegistryClient::StartGet(
            RegistryBaseUrl,
            RegistryToken,
            Entry.ReleaseId,
            RegistryRequestTimeoutSeconds,
            Error);
    RegistryRequestEntry = Entry;
    RegistryRequestProjectId = ProjectId;
    RegistryRequestProjectName = ProjectName;
    RegistryBuild = {};
    PendingRegistryOperation = EAkUGCReleaseRegistryOperation::Get;
    bRegistryChainCancelled = false;
    StartRegistryRequest(
        EAkUGCRegistryPanelRequest::GetRelease,
        Request,
        Error,
        TEXT("正在刷新 Registry Release 状态；当前项目和本地作品库不会被修改。"));
    return FReply::Handled();
}

FReply SAkUGCWorkLibraryPanel::RetryRegistryBuild()
{
    if (PendingRegistryRequest.IsValid()
        || !LastPublishedEntry.IsSet()
        || RegistryBuild.Status != TEXT("failed")
        || !RegistryBuild.bRetryable)
    {
        return FReply::Handled();
    }
    FString Error;
    FGuid ProjectId;
    FString ProjectName;
    if (!CurrentProjectIdentity(ProjectId, ProjectName, Error))
    {
        RegistryStatus = Error;
        return FReply::Handled();
    }
    const FAkUGCWorkEntry Entry = LastPublishedEntry.GetValue();
    if (!FAkUGCReleaseRegistryClient::ValidateSubmissionArtifact(
            Entry,
            LastPublishedSharePath,
            ProjectId,
            Error,
            &RegistryRequestArtifactSha256,
            &RegistryRequestArtifactSize))
    {
        RegistryStatus = Error;
        return FReply::Handled();
    }
    RegistryRequestEntry = Entry;
    RegistryRequestProjectId = ProjectId;
    RegistryRequestProjectName = ProjectName;
    bRegistryChainCancelled = false;
    StartBuildRequest(
        EAkUGCBuildRegistryOperation::Retry,
        TEXT("正在重新排队可重试构建；不会改写或重新签名本地制品。"));
    return FReply::Handled();
}

FReply SAkUGCWorkLibraryPanel::PublishToRegistry()
{
    if (PendingRegistryRequest.IsValid()
        || !LastPublishedEntry.IsSet()
        || RegistryReleaseStatus != TEXT("approved")
        || RegistryBuild.Status != TEXT("succeeded"))
    {
        return FReply::Handled();
    }
    FString Error;
    FGuid ProjectId;
    FString ProjectName;
    if (!CurrentProjectIdentity(ProjectId, ProjectName, Error))
    {
        RegistryStatus = Error;
        return FReply::Handled();
    }
    if (RegistryReleaseProjectId != ProjectId)
    {
        RegistryStatus =
            TEXT("已审核 Release 不属于当前 Document Project。");
        return FReply::Handled();
    }
    const FAkUGCWorkEntry Entry = LastPublishedEntry.GetValue();
    if (!FAkUGCReleaseRegistryClient::ValidateSubmissionArtifact(
            Entry,
            LastPublishedSharePath,
            ProjectId,
            Error,
            &RegistryRequestArtifactSha256,
            &RegistryRequestArtifactSize))
    {
        RegistryStatus = Error;
        return FReply::Handled();
    }
    if (RegistryBuild.ReleaseId != Entry.ReleaseId
        || RegistryBuild.ProjectId != ProjectId
        || RegistryBuild.InputArtifactSha256
            != RegistryRequestArtifactSha256
        || RegistryBuild.OutputArtifactSha256
            != RegistryRequestArtifactSha256
        || RegistryBuild.OutputArtifactSize
            != RegistryRequestArtifactSize)
    {
        RegistryStatus =
            TEXT("可信构建结果未与当前本地签名制品精确绑定，请刷新状态。");
        return FReply::Handled();
    }
    const FAkUGCEditorHttpRequestPtr Request =
        FAkUGCReleaseRegistryClient::StartPublish(
            RegistryBaseUrl,
            RegistryToken,
            Entry.ReleaseId,
            RegistryRequestTimeoutSeconds,
            Error);
    RegistryRequestEntry = Entry;
    RegistryRequestProjectId = ProjectId;
    RegistryRequestProjectName = ProjectName;
    PendingRegistryOperation = EAkUGCReleaseRegistryOperation::Publish;
    bRegistryChainCancelled = false;
    StartRegistryRequest(
        EAkUGCRegistryPanelRequest::PublishRelease,
        Request,
        Error,
        TEXT("正在发布已审核 Release；不会修改或重新签名本地制品。"));
    return FReply::Handled();
}

FReply SAkUGCWorkLibraryPanel::RefreshRegistryProject()
{
    if (PendingRegistryRequest.IsValid())
    {
        return FReply::Handled();
    }
    bRegistryChainCancelled = false;
    StartRefreshProjectRequest(
        TEXT("正在刷新云 Project、协作者和最新 100 条不可变版本。"));
    return FReply::Handled();
}

FReply SAkUGCWorkLibraryPanel::SetRegistryMember()
{
    if (PendingRegistryRequest.IsValid()
        || RegistryProjectAccessRole != TEXT("owner"))
    {
        return FReply::Handled();
    }
    FString Error;
    FGuid ProjectId;
    FString ProjectName;
    if (!CurrentProjectIdentity(ProjectId, ProjectName, Error)
        || RegistryProject.ProjectId != ProjectId)
    {
        RegistryStatus = Error.IsEmpty()
            ? TEXT("请先刷新当前云 Project。")
            : Error;
        return FReply::Handled();
    }
    const FString Subject = RegistryMemberSubject;
    const FString Role = RegistryMemberRole.TrimStartAndEnd().ToLower();
    const FAkUGCEditorHttpRequestPtr Request =
        FAkUGCReleaseRegistryClient::StartSetProjectMember(
            RegistryBaseUrl,
            RegistryToken,
            ProjectId,
            Subject,
            Role,
            RegistryRequestTimeoutSeconds,
            Error);
    RegistryRequestProjectId = ProjectId;
    RegistryRequestProjectName = ProjectName;
    RegistryRequestMemberSubject = Subject;
    RegistryRequestMemberRole = Role;
    PendingProjectOperation = EAkUGCProjectRegistryOperation::SetMember;
    bRegistryChainCancelled = false;
    StartRegistryRequest(
        EAkUGCRegistryPanelRequest::SetMember,
        Request,
        Error,
        TEXT("正在设置协作者；本地项目、作品库和签名制品不会被修改。"));
    return FReply::Handled();
}

FReply SAkUGCWorkLibraryPanel::RemoveRegistryMember()
{
    if (PendingRegistryRequest.IsValid()
        || RegistryProjectAccessRole != TEXT("owner"))
    {
        return FReply::Handled();
    }
    FString Error;
    FGuid ProjectId;
    FString ProjectName;
    if (!CurrentProjectIdentity(ProjectId, ProjectName, Error)
        || RegistryProject.ProjectId != ProjectId)
    {
        RegistryStatus = Error.IsEmpty()
            ? TEXT("请先刷新当前云 Project。")
            : Error;
        return FReply::Handled();
    }
    const FString Subject = RegistryMemberSubject;
    const FAkUGCEditorHttpRequestPtr Request =
        FAkUGCReleaseRegistryClient::StartRemoveProjectMember(
            RegistryBaseUrl,
            RegistryToken,
            ProjectId,
            Subject,
            RegistryRequestTimeoutSeconds,
            Error);
    RegistryRequestProjectId = ProjectId;
    RegistryRequestProjectName = ProjectName;
    RegistryRequestMemberSubject = Subject;
    RegistryRequestMemberRole.Reset();
    PendingProjectOperation = EAkUGCProjectRegistryOperation::RemoveMember;
    bRegistryChainCancelled = false;
    StartRegistryRequest(
        EAkUGCRegistryPanelRequest::RemoveMember,
        Request,
        Error,
        TEXT("正在移除协作者；本地项目、作品库和签名制品不会被修改。"));
    return FReply::Handled();
}

FReply SAkUGCWorkLibraryPanel::SetRegistryVisibility(
    const FString& Visibility)
{
    if (PendingRegistryRequest.IsValid()
        || RegistryProjectAccessRole != TEXT("owner"))
    {
        return FReply::Handled();
    }
    FString Error;
    FGuid ProjectId;
    FString ProjectName;
    if (!CurrentProjectIdentity(ProjectId, ProjectName, Error)
        || RegistryProject.ProjectId != ProjectId)
    {
        RegistryStatus = Error.IsEmpty()
            ? TEXT("请先刷新当前云 Project。")
            : Error;
        return FReply::Handled();
    }
    const FAkUGCEditorHttpRequestPtr Request =
        FAkUGCReleaseRegistryClient::StartSetProjectVisibility(
            RegistryBaseUrl,
            RegistryToken,
            ProjectId,
            Visibility,
            RegistryRequestTimeoutSeconds,
            Error);
    RegistryRequestProjectId = ProjectId;
    RegistryRequestProjectName = ProjectName;
    RegistryRequestVisibility = Visibility;
    PendingProjectOperation =
        EAkUGCProjectRegistryOperation::SetVisibility;
    bRegistryChainCancelled = false;
    StartRegistryRequest(
        EAkUGCRegistryPanelRequest::SetVisibility,
        Request,
        Error,
        Visibility == TEXT("public")
            ? TEXT("正在公开作品页；只有 published 版本会对项目外 Player 可见。")
            : TEXT("正在关闭公开作品页；项目外 Player 的元数据与下载入口将被隐藏。"));
    return FReply::Handled();
}

FReply SAkUGCWorkLibraryPanel::CancelRegistryRequest()
{
    if (PendingRegistryRequest.IsValid())
    {
        bRegistryChainCancelled = true;
        PendingRegistryRequest->Cancel();
        RegistryStatus = TEXT("正在取消 Registry 请求；不会继续后续请求链。");
    }
    return FReply::Handled();
}

void SAkUGCWorkLibraryPanel::CompleteRegistryRequest(
    const FAkUGCEditorHttpRequestPtr& Request,
    FAkUGCEditorHttpResult Result)
{
    if (Request != PendingRegistryRequest)
    {
        return;
    }
    PendingRegistryRequest.Reset();
    if (bRegistryChainCancelled || Result.bCancelled)
    {
        PendingRegistryRequestKind = EAkUGCRegistryPanelRequest::None;
        RegistryStatus =
            TEXT("Registry 请求已取消；后续请求链已停止，请刷新确认服务端状态。");
        return;
    }
    switch (PendingRegistryRequestKind)
    {
    case EAkUGCRegistryPanelRequest::SubmitRelease:
    case EAkUGCRegistryPanelRequest::GetRelease:
    case EAkUGCRegistryPanelRequest::PublishRelease:
        CompleteReleaseRegistryRequest(Result);
        break;
    case EAkUGCRegistryPanelRequest::GetBuild:
    case EAkUGCRegistryPanelRequest::RetryBuild:
        CompleteBuildRegistryRequest(Result);
        break;
    case EAkUGCRegistryPanelRequest::EnsureProject:
    case EAkUGCRegistryPanelRequest::CreateProject:
    case EAkUGCRegistryPanelRequest::RefreshProject:
    case EAkUGCRegistryPanelRequest::ListMembers:
    case EAkUGCRegistryPanelRequest::ListVersions:
    case EAkUGCRegistryPanelRequest::SetMember:
    case EAkUGCRegistryPanelRequest::RemoveMember:
    case EAkUGCRegistryPanelRequest::SetVisibility:
        CompleteProjectRegistryRequest(Result);
        break;
    default:
        RegistryStatus = TEXT("Registry 返回了未知操作结果。");
        PendingRegistryRequestKind = EAkUGCRegistryPanelRequest::None;
        break;
    }
}

bool SAkUGCWorkLibraryPanel::StartRegistryRequest(
    EAkUGCRegistryPanelRequest RequestKind,
    const FAkUGCEditorHttpRequestPtr& Request,
    const FString& Error,
    const FString& PendingStatus)
{
    if (!Request.IsValid())
    {
        PendingRegistryRequestKind = EAkUGCRegistryPanelRequest::None;
        RegistryStatus = Error.IsEmpty()
            ? TEXT("Registry 请求未能启动。")
            : Error;
        return false;
    }
    PendingRegistryRequestKind = RequestKind;
    PendingRegistryRequest = Request;
    RegistryStatus = PendingStatus;
    return true;
}

bool SAkUGCWorkLibraryPanel::CurrentProjectIdentity(
    FGuid& OutProjectId,
    FString& OutDisplayName,
    FString& OutError) const
{
    OutProjectId.Invalidate();
    OutDisplayName.Reset();
    OutError.Reset();
    if (!Subsystem.IsValid() || !Subsystem->HasOpenProject())
    {
        OutError = TEXT("请先打开项目。");
        return false;
    }
    const FAkUGCProjectManifest& Manifest =
        Subsystem->GetDocument().Manifest;
    OutProjectId = Manifest.ProjectId;
    OutDisplayName = Manifest.DisplayName;
    if (!OutProjectId.IsValid())
    {
        OutError = TEXT("当前文档 Project ID 无效。");
        return false;
    }
    return true;
}

bool SAkUGCWorkLibraryPanel::StartSubmitReleaseRequest()
{
    FString Error;
    PendingRegistryOperation = EAkUGCReleaseRegistryOperation::Submit;
    return StartRegistryRequest(
        EAkUGCRegistryPanelRequest::SubmitRelease,
        FAkUGCReleaseRegistryClient::StartSubmit(
            RegistryBaseUrl,
            RegistryToken,
            RegistryRequestEntry,
            LastPublishedSharePath,
            RegistryRequestProjectId,
            RegistryRequestTimeoutSeconds,
            Error),
        Error,
        TEXT("云 Project 已确认；正在上传本地验签通过的不可变签名包。"));
}

bool SAkUGCWorkLibraryPanel::StartBuildRequest(
    EAkUGCBuildRegistryOperation Operation,
    const FString& PendingStatus)
{
    FString Error;
    PendingBuildOperation = Operation;
    const FAkUGCEditorHttpRequestPtr Request =
        Operation == EAkUGCBuildRegistryOperation::Get
            ? FAkUGCReleaseRegistryClient::StartGetBuild(
                RegistryBaseUrl,
                RegistryToken,
                RegistryRequestEntry.ReleaseId,
                RegistryRequestTimeoutSeconds,
                Error)
            : FAkUGCReleaseRegistryClient::StartRetryBuild(
                RegistryBaseUrl,
                RegistryToken,
                RegistryRequestEntry.ReleaseId,
                RegistryRequestTimeoutSeconds,
                Error);
    return StartRegistryRequest(
        Operation == EAkUGCBuildRegistryOperation::Get
            ? EAkUGCRegistryPanelRequest::GetBuild
            : EAkUGCRegistryPanelRequest::RetryBuild,
        Request,
        Error,
        PendingStatus);
}

bool SAkUGCWorkLibraryPanel::StartRefreshProjectRequest(
    const FString& PendingStatus)
{
    FString Error;
    FGuid ProjectId;
    FString ProjectName;
    if (!CurrentProjectIdentity(ProjectId, ProjectName, Error))
    {
        PendingRegistryRequestKind = EAkUGCRegistryPanelRequest::None;
        RegistryStatus = Error;
        return false;
    }
    if (RegistryProject.ProjectId.IsValid()
        && RegistryProject.ProjectId != ProjectId)
    {
        ClearRegistryProjectView();
    }
    RegistryRequestProjectId = ProjectId;
    RegistryRequestProjectName = ProjectName;
    PendingProjectOperation = EAkUGCProjectRegistryOperation::GetProject;
    return StartRegistryRequest(
        EAkUGCRegistryPanelRequest::RefreshProject,
        FAkUGCReleaseRegistryClient::StartGetProject(
            RegistryBaseUrl,
            RegistryToken,
            ProjectId,
            RegistryRequestTimeoutSeconds,
            Error),
        Error,
        PendingStatus);
}

void SAkUGCWorkLibraryPanel::CompleteReleaseRegistryRequest(
    const FAkUGCEditorHttpResult& Result)
{
    FAkUGCReleaseRegistryResponse Response;
    FString Error;
    if (!FAkUGCReleaseRegistryClient::ParseResponse(
            PendingRegistryOperation,
            Result,
            RegistryRequestEntry,
            RegistryRequestProjectId,
            Response,
            Error))
    {
        RegistryStatus = Error;
        PendingRegistryRequestKind = EAkUGCRegistryPanelRequest::None;
        return;
    }
    if (Response.Release.ArtifactSha256
            != RegistryRequestArtifactSha256
        || Response.Release.ArtifactSize
            != RegistryRequestArtifactSize)
    {
        RegistryStatus =
            TEXT("Registry Release 制品摘要或大小与本地签名字节不一致。");
        PendingRegistryRequestKind = EAkUGCRegistryPanelRequest::None;
        return;
    }
    RegistryReleaseStatus = Response.Release.Status;
    RegistryReleaseArtifactSha256 =
        Response.Release.ArtifactSha256;
    RegistryReleaseArtifactSize = Response.Release.ArtifactSize;
    RegistryReleaseProjectId = Response.Release.ProjectId;
    const FString Id =
        Response.Release.ReleaseId.ToString(EGuidFormats::DigitsWithHyphensLower);
    switch (PendingRegistryOperation)
    {
    case EAkUGCReleaseRegistryOperation::Submit:
        RegistryStatus = FString::Printf(
            TEXT("%s：%s\nRegistry 状态：%s；正在读取异步可信构建。"),
            Response.bCreated ? TEXT("Registry 已创建不可变 Release") : TEXT("Registry 幂等返回现有 Release"),
            *Id,
            *RegistryReleaseStatus);
        StartBuildRequest(
            EAkUGCBuildRegistryOperation::Get,
            RegistryStatus);
        return;
    case EAkUGCReleaseRegistryOperation::Get:
        RegistryStatus = FString::Printf(
            TEXT("Registry Release 已刷新：%s\n状态：%s，修订：%lld；正在读取可信构建。"),
            *Id,
            *RegistryReleaseStatus,
            static_cast<long long>(Response.Release.Revision));
        StartBuildRequest(
            EAkUGCBuildRegistryOperation::Get,
            RegistryStatus);
        return;
    case EAkUGCReleaseRegistryOperation::Publish:
        RegistryStatus = FString::Printf(
            TEXT("%s：%s\nPlayer 下载后仍必须使用独立可信公钥本地验签。"),
            Response.bChanged ? TEXT("Release 已发布") : TEXT("Release 已是 published，幂等成功"),
            *Id);
        break;
    default:
        RegistryStatus = TEXT("Registry 返回了未知操作结果。");
        break;
    }
    PendingRegistryRequestKind = EAkUGCRegistryPanelRequest::None;
}

void SAkUGCWorkLibraryPanel::CompleteBuildRegistryRequest(
    const FAkUGCEditorHttpResult& Result)
{
    FAkUGCBuildRegistryResponse Response;
    FString Error;
    if (!FAkUGCReleaseRegistryClient::ParseBuildResponse(
            PendingBuildOperation,
            Result,
            RegistryRequestEntry.ReleaseId,
            RegistryRequestProjectId,
            RegistryRequestArtifactSha256,
            RegistryRequestArtifactSize,
            Response,
            Error))
    {
        RegistryBuild = {};
        RegistryStatus = Error;
        PendingRegistryRequestKind = EAkUGCRegistryPanelRequest::None;
        return;
    }
    RegistryBuild = MoveTemp(Response.Build);
    const FString Id = RegistryBuild.ReleaseId.ToString(
        EGuidFormats::DigitsWithHyphensLower);
    if (PendingBuildOperation == EAkUGCBuildRegistryOperation::Retry)
    {
        if (RegistryBuild.Status != TEXT("queued"))
        {
            RegistryBuild = {};
            RegistryStatus =
                TEXT("Registry 重试响应未将构建恢复为 queued。");
            PendingRegistryRequestKind =
                EAkUGCRegistryPanelRequest::None;
            return;
        }
        RegistryStatus = FString::Printf(
            TEXT("%s：%s\n构建已排队；服务端只会深度校验同一签名字节。"),
            Response.bChanged
                ? TEXT("可信构建已重新排队")
                : TEXT("可信构建已经在队列中"),
            *Id);
    }
    else if (RegistryBuild.Status == TEXT("succeeded"))
    {
        RegistryStatus = FString::Printf(
            TEXT("可信构建成功：%s\n制品摘要与大小已绑定本地签名字节；Reviewer 仍必须是另一主体。"),
            *Id);
    }
    else if (RegistryBuild.Status == TEXT("failed"))
    {
        RegistryStatus = FString::Printf(
            TEXT("可信构建失败：%s\n错误：%s；%s。"),
            *Id,
            *RegistryBuild.ErrorCode,
            RegistryBuild.bRetryable
                ? TEXT("可显式重新排队")
                : TEXT("该签名 Release 不可重试，需修复后生成新 Release"));
    }
    else
    {
        RegistryStatus = FString::Printf(
            TEXT("可信构建状态：%s\nRelease：%s；尝试 %lld。"),
            *RegistryBuild.Status,
            *Id,
            static_cast<long long>(RegistryBuild.Attempt));
    }
    PendingRegistryRequestKind = EAkUGCRegistryPanelRequest::None;
}

void SAkUGCWorkLibraryPanel::CompleteProjectRegistryRequest(
    const FAkUGCEditorHttpResult& Result)
{
    const EAkUGCRegistryPanelRequest CompletedKind =
        PendingRegistryRequestKind;
    FString Error;
    if (CompletedKind == EAkUGCRegistryPanelRequest::EnsureProject
        && Result.NativeCode == 0
        && !Result.bTimedOut
        && !Result.bTooLarge
        && !Result.bHeadersTooLarge
        && Result.StatusCode == 404)
    {
        PendingProjectOperation =
            EAkUGCProjectRegistryOperation::CreateProject;
        StartRegistryRequest(
            EAkUGCRegistryPanelRequest::CreateProject,
            FAkUGCReleaseRegistryClient::StartCreateProject(
                RegistryBaseUrl,
                RegistryToken,
                RegistryRequestProjectId,
                RegistryRequestProjectName,
                RegistryRequestTimeoutSeconds,
                Error),
            Error,
            TEXT("云 Project 不存在；正在以当前身份登记。"));
        return;
    }
    if (CompletedKind == EAkUGCRegistryPanelRequest::CreateProject
        && Result.NativeCode == 0
        && !Result.bTimedOut
        && !Result.bTooLarge
        && !Result.bHeadersTooLarge
        && Result.StatusCode == 409)
    {
        PendingProjectOperation =
            EAkUGCProjectRegistryOperation::GetProject;
        StartRegistryRequest(
            EAkUGCRegistryPanelRequest::CreateProject,
            FAkUGCReleaseRegistryClient::StartGetProject(
                RegistryBaseUrl,
                RegistryToken,
                RegistryRequestProjectId,
                RegistryRequestTimeoutSeconds,
                Error),
            Error,
            TEXT("Project 登记发生并发冲突；正在重新读取权限后决定是否上传。"));
        return;
    }

    FAkUGCProjectRegistryResponse Response;
    if (!FAkUGCReleaseRegistryClient::ParseProjectResponse(
            PendingProjectOperation,
            Result,
            RegistryRequestProjectId,
            Response,
            Error))
    {
        RegistryStatus = Error;
        PendingRegistryRequestKind = EAkUGCRegistryPanelRequest::None;
        return;
    }

    switch (CompletedKind)
    {
    case EAkUGCRegistryPanelRequest::EnsureProject:
    case EAkUGCRegistryPanelRequest::CreateProject:
        if (CompletedKind == EAkUGCRegistryPanelRequest::CreateProject
            && PendingProjectOperation
                == EAkUGCProjectRegistryOperation::CreateProject
            && Response.Project.DisplayName != RegistryRequestProjectName)
        {
            RegistryStatus =
                TEXT("Registry 创建结果的 Project 名称与当前文档不一致。");
            PendingRegistryRequestKind = EAkUGCRegistryPanelRequest::None;
            return;
        }
        RegistryProject = Response.Project;
        RegistryProjectAccessRole = Response.AccessRole;
        UpdateRegistryProjectSummary();
        if (RegistryProjectAccessRole != TEXT("owner")
            && RegistryProjectAccessRole != TEXT("editor"))
        {
            RegistryStatus =
                TEXT("当前主体只有 viewer 权限，拒绝上传签名包。");
            PendingRegistryRequestKind = EAkUGCRegistryPanelRequest::None;
            return;
        }
        StartSubmitReleaseRequest();
        return;
    case EAkUGCRegistryPanelRequest::RefreshProject:
        RegistryProject = Response.Project;
        RegistryProjectAccessRole = Response.AccessRole;
        UpdateRegistryProjectSummary();
        PendingProjectOperation =
            EAkUGCProjectRegistryOperation::ListMembers;
        StartRegistryRequest(
            EAkUGCRegistryPanelRequest::ListMembers,
            FAkUGCReleaseRegistryClient::StartListProjectMembers(
                RegistryBaseUrl,
                RegistryToken,
                RegistryRequestProjectId,
                RegistryRequestTimeoutSeconds,
                Error),
            Error,
            TEXT("云 Project 已刷新；正在读取协作者。"));
        return;
    case EAkUGCRegistryPanelRequest::ListMembers:
        if (!Response.Members.ContainsByPredicate(
                [this](const FAkUGCRegistryMemberView& Member)
                {
                    return Member.Role == TEXT("owner")
                        && Member.Subject == RegistryProject.OwnerSubject;
                }))
        {
            RegistryStatus =
                TEXT("Registry 协作者列表中的 Owner 与 Project 不一致。");
            PendingRegistryRequestKind = EAkUGCRegistryPanelRequest::None;
            return;
        }
        RegistryMembers = MoveTemp(Response.Members);
        UpdateRegistryProjectSummary();
        PendingProjectOperation =
            EAkUGCProjectRegistryOperation::ListVersions;
        StartRegistryRequest(
            EAkUGCRegistryPanelRequest::ListVersions,
            FAkUGCReleaseRegistryClient::StartListProjectVersions(
                RegistryBaseUrl,
                RegistryToken,
                RegistryRequestProjectId,
                FAkUGCReleaseRegistryClient::MaxVersionsPerPage,
                RegistryRequestTimeoutSeconds,
                Error),
            Error,
            TEXT("协作者已刷新；正在读取最新 100 条不可变版本。"));
        return;
    case EAkUGCRegistryPanelRequest::ListVersions:
        RegistryVersions = MoveTemp(Response.Versions);
        RegistryVersionsNextCursor = MoveTemp(Response.NextCursor);
        UpdateRegistryProjectSummary();
        RegistryStatus =
            RegistryVersionsNextCursor.IsEmpty()
                ? TEXT("云 Project、协作者和版本历史已刷新。")
                : TEXT("云 Project 已刷新；版本历史超过 100 条，当前仅显示最新 100 条。");
        PendingRegistryRequestKind = EAkUGCRegistryPanelRequest::None;
        return;
    case EAkUGCRegistryPanelRequest::SetMember:
        if (Response.Member.Subject != RegistryRequestMemberSubject
            || Response.Member.Role != RegistryRequestMemberRole)
        {
            RegistryStatus =
                TEXT("Registry 设置协作者响应与请求目标不一致。");
            PendingRegistryRequestKind = EAkUGCRegistryPanelRequest::None;
            return;
        }
        RegistryProject = Response.Project;
        RegistryProjectAccessRole = Response.AccessRole;
        RegistryStatus = Response.bChanged
            ? TEXT("协作者权限已更新；正在刷新云 Project。")
            : TEXT("协作者权限未变化；正在刷新云 Project。");
        StartRefreshProjectRequest(RegistryStatus);
        return;
    case EAkUGCRegistryPanelRequest::RemoveMember:
        RegistryProject = Response.Project;
        RegistryProjectAccessRole = Response.AccessRole;
        RegistryStatus = Response.bChanged
            ? TEXT("协作者已移除；正在刷新云 Project。")
            : TEXT("协作者不存在；正在刷新云 Project。");
        StartRefreshProjectRequest(RegistryStatus);
        return;
    case EAkUGCRegistryPanelRequest::SetVisibility:
        if (Response.Project.Visibility != RegistryRequestVisibility)
        {
            RegistryStatus =
                TEXT("Registry 可见性响应与请求目标不一致。");
            PendingRegistryRequestKind = EAkUGCRegistryPanelRequest::None;
            return;
        }
        RegistryProject = Response.Project;
        RegistryProjectAccessRole = Response.AccessRole;
        RegistryStatus = Response.bChanged
            ? (RegistryProject.Visibility == TEXT("public")
                ? TEXT("作品页已公开；正在刷新云 Project。")
                : TEXT("作品页已设为私人；正在刷新云 Project。"))
            : TEXT("作品可见性未变化；正在刷新云 Project。");
        StartRefreshProjectRequest(RegistryStatus);
        return;
    default:
        RegistryStatus = TEXT("Registry 返回了未知 Project 操作结果。");
        PendingRegistryRequestKind = EAkUGCRegistryPanelRequest::None;
        return;
    }
}

void SAkUGCWorkLibraryPanel::ClearRegistryProjectView()
{
    RegistryProject = {};
    RegistryProjectAccessRole.Reset();
    RegistryMembers.Reset();
    RegistryVersions.Reset();
    RegistryVersionsNextCursor.Reset();
    RegistryProjectSummary.Reset();
    RegistryMembersSummary.Reset();
    RegistryVersionsSummary.Reset();
}

void SAkUGCWorkLibraryPanel::UpdateRegistryProjectSummary()
{
    if (!RegistryProject.ProjectId.IsValid())
    {
        RegistryProjectSummary.Reset();
    }
    else
    {
        RegistryProjectSummary = FString::Printf(
            TEXT("云 Project：%s\nID：%s\nOwner：%s；当前权限：%s；可见性：%s；修订：%lld"),
            *RegistryProject.DisplayName,
            *RegistryProject.ProjectId.ToString(
                EGuidFormats::DigitsWithHyphensLower),
            *RegistryProject.OwnerSubject,
            *RegistryProjectAccessRole,
            *RegistryProject.Visibility,
            static_cast<long long>(RegistryProject.Revision));
    }

    RegistryMembersSummary = FString::Printf(
        TEXT("协作者（%d）"),
        RegistryMembers.Num());
    for (const FAkUGCRegistryMemberView& Member : RegistryMembers)
    {
        RegistryMembersSummary += FString::Printf(
            TEXT("\n%s：%s"),
            *Member.Subject,
            *Member.Role);
    }

    RegistryVersionsSummary = FString::Printf(
        TEXT("不可变版本（当前页 %d%s）"),
        RegistryVersions.Num(),
        RegistryVersionsNextCursor.IsEmpty()
            ? TEXT("")
            : TEXT("，还有更早版本，已截断"));
    for (const FAkUGCReleaseRegistryView& Version : RegistryVersions)
    {
        RegistryVersionsSummary += FString::Printf(
            TEXT("\nR%lld  %s  %s  提交者 %s  %s"),
            static_cast<long long>(Version.Revision),
            *Version.ReleaseId.ToString(EGuidFormats::DigitsWithHyphensLower),
            *Version.Status,
            *Version.SubmittedBySubject,
            *Version.SubmittedAt);
    }
}

bool SAkUGCWorkLibraryPanel::ReadRecoveryPassphrase(
    bool bRequireConfirmation,
    TArray<uint8>& OutPassphrase,
    FString& OutError) const
{
    OutPassphrase.Reset();
    OutError.Reset();
    if (!RecoveryPassphraseBox.IsValid()
        || (bRequireConfirmation && !RecoveryPassphraseConfirmBox.IsValid()))
    {
        OutError = TEXT("身份恢复密码输入控件不可用。");
        return false;
    }

    FString Password = RecoveryPassphraseBox->GetText().ToString();
    FString Confirmation = RecoveryPassphraseConfirmBox.IsValid()
        ? RecoveryPassphraseConfirmBox->GetText().ToString()
        : FString();
    if (bRequireConfirmation && Password != Confirmation)
    {
        SecureResetString(Password);
        SecureResetString(Confirmation);
        OutError = TEXT("两次输入的身份恢复密码不一致。");
        return false;
    }
    FTCHARToUTF8 PasswordUtf8(*Password);
    if (PasswordUtf8.Length() > 0)
    {
        OutPassphrase.Append(
            reinterpret_cast<const uint8*>(PasswordUtf8.Get()),
            PasswordUtf8.Length());
        FMemory::Memzero(
            const_cast<ANSICHAR*>(PasswordUtf8.Get()),
            PasswordUtf8.Length());
    }
    SecureResetString(Password);
    SecureResetString(Confirmation);
    if (OutPassphrase.Num() < 12 || OutPassphrase.Num() > 1024)
    {
        if (!OutPassphrase.IsEmpty())
        {
            FMemory::Memzero(OutPassphrase.GetData(), OutPassphrase.Num());
        }
        OutPassphrase.Reset();
        OutError = TEXT("身份恢复密码必须是 12 至 1024 字节的 UTF-8 文本。");
        return false;
    }
    return true;
}

void SAkUGCWorkLibraryPanel::ClearRecoveryPassphrase()
{
    if (RecoveryPassphraseBox.IsValid())
    {
        RecoveryPassphraseBox->SetText(FText::GetEmpty());
    }
    if (RecoveryPassphraseConfirmBox.IsValid())
    {
        RecoveryPassphraseConfirmBox->SetText(FText::GetEmpty());
    }
}

FReply SAkUGCWorkLibraryPanel::BackupIdentity()
{
    TArray<uint8> Passphrase;
    FString Error;
    const bool bPassphraseReady =
        ReadRecoveryPassphrase(true, Passphrase, Error);
    const bool bExported = bPassphraseReady
        && FAkUGCPublisherKeyStore(
            FAkUGCPublisherKeyStore::DefaultDirectory()).ExportRecoveryBundle(
                RecoveryPath,
                Passphrase,
                &Error);
    if (!Passphrase.IsEmpty())
    {
        FMemory::Memzero(Passphrase.GetData(), Passphrase.Num());
    }
    Passphrase.Reset();
    ClearRecoveryPassphrase();
    Status = bExported
        ? TEXT("发布身份加密备份已导出：") + FPaths::ConvertRelativePathToFull(RecoveryPath)
            + TEXT("\n请将恢复包与密码分开保存；两者缺一均无法恢复。")
        : Error;
    return FReply::Handled();
}

FReply SAkUGCWorkLibraryPanel::RestoreIdentity()
{
    TArray<uint8> Passphrase;
    FString Error;
    FAkUGCPublisherIdentityInfo Info;
    const bool bPassphraseReady =
        ReadRecoveryPassphrase(false, Passphrase, Error);
    const bool bRestored = bPassphraseReady
        && FAkUGCPublisherKeyStore(
            FAkUGCPublisherKeyStore::DefaultDirectory()).ImportRecoveryBundle(
                RecoveryPath,
                Passphrase,
                Info,
                &Error);
    if (!Passphrase.IsEmpty())
    {
        FMemory::Memzero(Passphrase.GetData(), Passphrase.Num());
    }
    Passphrase.Reset();
    ClearRecoveryPassphrase();
    Status = bRestored
        ? FString::Printf(
            TEXT("发布身份恢复完成：公钥 %s，密钥版本 %d。"),
            *Info.PublicKey,
            Info.KeyVersion)
        : Error;
    return FReply::Handled();
}

FReply SAkUGCWorkLibraryPanel::RotateIdentity()
{
    if ((FApp::IsUnattended() && !FApp::AllowUnattendedInput())
        || !bConfirmedIdentityRotation)
    {
        bConfirmedIdentityRotation = false;
        ClearRecoveryPassphrase();
        Status = TEXT("轮换需要交互环境并显式确认；当前身份未改变。");
        return FReply::Handled();
    }

    TArray<uint8> Passphrase;
    FString Error;
    FAkUGCPublisherIdentityInfo Info;
    const bool bPassphraseReady =
        ReadRecoveryPassphrase(true, Passphrase, Error);
    const bool bRotated = bPassphraseReady
        && FAkUGCPublisherKeyStore(
            FAkUGCPublisherKeyStore::DefaultDirectory()).RotateWithRecovery(
                RecoveryPath,
                Passphrase,
                Info,
                &Error);
    if (!Passphrase.IsEmpty())
    {
        FMemory::Memzero(Passphrase.GetData(), Passphrase.Num());
    }
    Passphrase.Reset();
    ClearRecoveryPassphrase();
    bConfirmedIdentityRotation = false;
    Status = bRotated
        ? FString::Printf(
            TEXT("发布身份已轮换到版本 %d：%s\n新身份恢复包：%s\n旧作品继续使用历史公钥验签；请先向 Registry 提交轮换链再发布新作品。"),
            Info.KeyVersion,
            *Info.PublicKey,
            *FPaths::ConvertRelativePathToFull(RecoveryPath))
        : Error;
    return FReply::Handled();
}

FReply SAkUGCWorkLibraryPanel::Import()
{
    if (!bConfirmedTrust) { Status = TEXT("请先独立核对并确认可信公钥。"); return FReply::Handled(); }
    FAkUGCWorkEntry Entry;
    FString Error;
    const bool Imported = FAkUGCWorkLibrary(FAkUGCWorkLibrary::DefaultDirectory()).Import(SharePath, TrustedKey, Entry, &Error);
    bConfirmedTrust = false;
    if (Imported) { Refresh(); Status = TEXT("验签导入成功；现有编辑项目未改变。可选择导出、删除或复制游玩ID。"); }
    else { Status = Error; }
    return FReply::Handled();
}

FReply SAkUGCWorkLibraryPanel::Refresh()
{
    Rows->ClearChildren();
    TArray<FAkUGCWorkEntry> Entries;
    int32 Rejected = 0;
    FAkUGCWorkLibrary Library(FAkUGCWorkLibrary::DefaultDirectory());
    FString Error;
    if (!Library.List(Entries, Rejected, &Error)) { Status = Error; return FReply::Handled(); }
    Status = FString::Printf(TEXT("本地作品 %d 个；隔离损坏或验签失败记录 %d 个。游玩端控制台使用 PlayWork <发布ID>。"), Entries.Num(), Rejected);
    for (const auto& Entry : Entries)
    {
        Rows->AddSlot().AutoHeight().Padding(0, 3)
        [SNew(SBorder)
            [SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight()
                [SNew(STextBlock).Text(FText::FromString(Entry.DisplayName + TEXT("\n") + Entry.ReleaseId.ToString(EGuidFormats::Digits))).AutoWrapText(true)]
                + SVerticalBox::Slot().AutoHeight()
                [SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().Padding(2)
                    [SNew(SButton).Text(FText::FromString(TEXT("复制游玩命令"))).OnClicked_Lambda([Entry]
                        { FPlatformApplicationMisc::ClipboardCopy(*(TEXT("PlayWork ") + Entry.ReleaseId.ToString(EGuidFormats::Digits))); return FReply::Handled(); })]
                    + SHorizontalBox::Slot().AutoWidth().Padding(2)
                    [SNew(SButton).Tag(FName(*(TEXT("导出分享:") + Entry.ReleaseId.ToString(EGuidFormats::Digits))))
                        .Text(FText::FromString(TEXT("导出分享"))).OnClicked_Lambda([this, Entry]
                        { FString Error; const FString Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UGC/Share"), Entry.ReleaseId.ToString(EGuidFormats::Digits) + TEXT("_") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".akugc"));
                          Status = FAkUGCWorkLibrary(FAkUGCWorkLibrary::DefaultDirectory()).Export(Entry.ReleaseId, Path, &Error) ? TEXT("已导出：") + FPaths::ConvertRelativePathToFull(Path) : Error;
                          return FReply::Handled(); })]
                    + SHorizontalBox::Slot().AutoWidth().Padding(2)
                    [SNew(SButton).Tag(FName(*(TEXT("删除本地记录:") + Entry.ReleaseId.ToString(EGuidFormats::Digits))))
                        .Text(FText::FromString(TEXT("删除本地记录"))).OnClicked_Lambda([this, Entry] { return RequestDelete(Entry); })]
                ]
            ]
        ];
    }
    return FReply::Handled();
}

FReply SAkUGCWorkLibraryPanel::RequestDelete(const FAkUGCWorkEntry& Entry)
{
    if (FApp::IsUnattended() && !FApp::AllowUnattendedInput()) { Status = TEXT("无人值守模式不确认删除；本地记录未改变。"); return FReply::Handled(); }
    if (PendingDeleteId.IsValid()) { return FReply::Handled(); }
    PendingDeleteId = Entry.ReleaseId;
    PendingDeleteName = Entry.DisplayName;
    return FReply::Handled();
}

FReply SAkUGCWorkLibraryPanel::CancelDelete()
{
    PendingDeleteId.Invalidate();
    PendingDeleteName.Reset();
    Status = TEXT("已取消删除；本地记录未改变。");
    return FReply::Handled();
}

FReply SAkUGCWorkLibraryPanel::ConfirmDelete()
{
    if ((FApp::IsUnattended() && !FApp::AllowUnattendedInput()) || !PendingDeleteId.IsValid()) { return FReply::Handled(); }
    const FGuid Id = PendingDeleteId;
    PendingDeleteId.Invalidate();
    PendingDeleteName.Reset();
    FString Error;
    if (FAkUGCWorkLibrary(FAkUGCWorkLibrary::DefaultDirectory()).Remove(Id, &Error))
    { Refresh(); Status = TEXT("已删除本地记录；分享文件和编辑项目未改变。"); }
    else { Status = Error; }
    return FReply::Handled();
}

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
                .IsEnabled_Lambda([this] { return Subsystem.IsValid() && Subsystem->HasOpenProject(); })
                .OnClicked(this, &SAkUGCWorkLibraryPanel::Publish)]
            + SHorizontalBox::Slot().AutoWidth().Padding(2)
            [SNew(SButton).Text(FText::FromString(TEXT("刷新作品库"))).OnClicked(this, &SAkUGCWorkLibraryPanel::Refresh)]
            + SHorizontalBox::Slot().AutoWidth().Padding(2)
            [SNew(SButton).Text(FText::FromString(TEXT("打开分享目录"))).OnClicked_Lambda([]
                { FPlatformProcess::ExploreFolder(*FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UGC/Share")))); return FReply::Handled(); })]
        ]
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
        bConfirmedTrust = false;
        Refresh();
        Status = FString::Printf(TEXT("发布完成：%s\n分享路径：%s\n独立核对公钥：%s\n已发布的是当前项目快照，不含尚未应用的参数草稿。"),
            *Entry.ReleaseId.ToString(EGuidFormats::Digits), *Path, *Entry.TrustedPublicKey);
    }
    else { Status = Error.IsEmpty() ? TEXT("请先打开项目。") : Error; }
    return FReply::Handled();
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

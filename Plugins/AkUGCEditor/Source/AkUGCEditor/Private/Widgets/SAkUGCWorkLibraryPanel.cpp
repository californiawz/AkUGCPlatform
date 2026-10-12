#include "Widgets/SAkUGCWorkLibraryPanel.h"

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "Subsystem/AkUGCEditorSubsystem.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

void SAkUGCWorkLibraryPanel::Construct(const FArguments& InArgs, UAkUGCEditorSubsystem* InSubsystem)
{
    Subsystem = InSubsystem;
    ChildSlot
    [
        SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight().Padding(0, 4)
        [SNew(STextBlock).Text(FText::FromString(TEXT("本地发布 / 分享 / 作品库")))]
        + SVerticalBox::Slot().AutoHeight()
        [SNew(STextBlock).Text(FText::FromString(TEXT("分享文件不含私钥。公钥须通过独立渠道核对；确认的是密钥，不是作者实名。发布私钥仅保存在本机 Saved/UGC/Publisher，未加密，请勿分享此目录。"))).AutoWrapText(true)]
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
    FString PrivateKey, Error, Path;
    FAkUGCWorkEntry Entry;
    if (Subsystem.IsValid() && Subsystem->HasOpenProject()
        && FAkUGCWorkLibrary::GetOrCreatePublisherKey(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UGC/Publisher/private.key")), PrivateKey, &Error)
        && FAkUGCWorkLibrary(FAkUGCWorkLibrary::DefaultDirectory()).Publish(Subsystem->GetDocument(), PrivateKey,
            FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UGC/Share")), Entry, Path, &Error))
    {
        SharePath = Path;
        bConfirmedTrust = false;
        Refresh();
        Status = FString::Printf(TEXT("发布完成：%s\n分享路径：%s\n独立核对公钥：%s\n已发布的是当前项目快照，不含尚未应用的参数草稿。"),
            *Entry.ReleaseId.ToString(EGuidFormats::Digits), *Path, *Entry.TrustedPublicKey);
    }
    else { Status = Error.IsEmpty() ? TEXT("请先打开项目。") : Error; }
    PrivateKey.Reset();
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

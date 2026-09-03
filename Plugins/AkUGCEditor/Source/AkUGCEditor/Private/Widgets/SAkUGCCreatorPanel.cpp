#include "Widgets/SAkUGCCreatorPanel.h"

#include "Prefab/AkUGCOfficialPrefabCatalog.h"
#include "Subsystem/AkUGCEditorSubsystem.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

void SAkUGCCreatorPanel::Construct(const FArguments& InArgs, UAkUGCEditorSubsystem* InSubsystem)
{
    Subsystem = InSubsystem;
    Status = TEXT("Create or load a UGC project.");

    TSharedRef<SVerticalBox> PrefabButtons = SNew(SVerticalBox);
    for (const FName PrefabId : FAkUGCOfficialPrefabCatalog::GetTowerDefensePrefabIds())
    {
        PrefabButtons->AddSlot()
        .AutoHeight()
        .Padding(0.0f, 2.0f)
        [
            SNew(SButton)
            .Text(FText::FromName(PrefabId))
            .OnClicked_Lambda([this, PrefabId]() { return PlacePrefab(PrefabId); })
        ];
    }

    ChildSlot
    [
        SNew(SBorder)
        .Padding(8.0f)
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot()
            .AutoHeight()
            [
                SNew(STextBlock)
                .Text(FText::FromString(TEXT("AkUGC Creator Studio - Phase 0")))
            ]
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.0f, 6.0f)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth().Padding(2.0f)
                [SNew(SButton).Text(FText::FromString(TEXT("New Tower Defense"))).OnClicked(this, &SAkUGCCreatorPanel::NewProject)]
                + SHorizontalBox::Slot().AutoWidth().Padding(2.0f)
                [SNew(SButton).Text(FText::FromString(TEXT("Save"))).OnClicked(this, &SAkUGCCreatorPanel::SaveProject)]
                + SHorizontalBox::Slot().AutoWidth().Padding(2.0f)
                [SNew(SButton).Text(FText::FromString(TEXT("Load"))).OnClicked(this, &SAkUGCCreatorPanel::LoadProject)]
                + SHorizontalBox::Slot().AutoWidth().Padding(2.0f)
                [SNew(SButton).Text(FText::FromString(TEXT("Undo"))).OnClicked(this, &SAkUGCCreatorPanel::Undo)]
                + SHorizontalBox::Slot().AutoWidth().Padding(2.0f)
                [SNew(SButton).Text(FText::FromString(TEXT("Redo"))).OnClicked(this, &SAkUGCCreatorPanel::Redo)]
                + SHorizontalBox::Slot().AutoWidth().Padding(2.0f)
                [SNew(SButton).Text(FText::FromString(TEXT("Duplicate Selected"))).OnClicked(this, &SAkUGCCreatorPanel::DuplicateSelected)]
                + SHorizontalBox::Slot().AutoWidth().Padding(2.0f)
                [SNew(SButton).Text(FText::FromString(TEXT("Delete Selected"))).OnClicked(this, &SAkUGCCreatorPanel::DeleteSelected)]
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f)
            [SNew(SSeparator)]
            + SVerticalBox::Slot().AutoHeight()
            [SNew(STextBlock).Text(FText::FromString(TEXT("Official Tower Defense Prefabs")))]
            + SVerticalBox::Slot().FillHeight(1.0f).Padding(0.0f, 4.0f)
            [SNew(SScrollBox) + SScrollBox::Slot()[PrefabButtons]]
            + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f)
            [SNew(STextBlock).Text(this, &SAkUGCCreatorPanel::GetStatusText).AutoWrapText(true)]
        ]
    ];
}

FReply SAkUGCCreatorPanel::NewProject()
{
    FString Error;
    Status = Subsystem.IsValid() && Subsystem->NewTowerDefenseProject(&Error)
        ? TEXT("Created a new tower defense project.")
        : Error;
    PlacementIndex = 0;
    return FReply::Handled();
}

FReply SAkUGCCreatorPanel::SaveProject()
{
    FString Error;
    const FString Path = Subsystem.IsValid() ? Subsystem->GetDefaultProjectPath() : FString{};
    Status = Subsystem.IsValid() && Subsystem->SaveProject(Path, &Error)
        ? FString::Printf(TEXT("Saved: %s"), *Path)
        : Error;
    return FReply::Handled();
}

FReply SAkUGCCreatorPanel::LoadProject()
{
    FString Error;
    const FString Path = Subsystem.IsValid() ? Subsystem->GetDefaultProjectPath() : FString{};
    Status = Subsystem.IsValid() && Subsystem->LoadProject(Path, &Error)
        ? FString::Printf(TEXT("Loaded: %s"), *Path)
        : Error;
    return FReply::Handled();
}

FReply SAkUGCCreatorPanel::Undo()
{
    if (Subsystem.IsValid())
    {
        const FAkUGCCommandExecutionResult Result = Subsystem->Undo();
        Status = Result.bSucceeded ? TEXT("Undo completed.") : Result.ErrorMessage;
    }
    return FReply::Handled();
}

FReply SAkUGCCreatorPanel::Redo()
{
    if (Subsystem.IsValid())
    {
        const FAkUGCCommandExecutionResult Result = Subsystem->Redo();
        Status = Result.bSucceeded ? TEXT("Redo completed.") : Result.ErrorMessage;
    }
    return FReply::Handled();
}

FReply SAkUGCCreatorPanel::DeleteSelected()
{
    if (Subsystem.IsValid())
    {
        const FAkUGCCommandExecutionResult Result = Subsystem->DeleteSelectedEntity();
        Status = Result.bSucceeded ? TEXT("Deleted selected UGC entity.") : Result.ErrorMessage;
    }
    return FReply::Handled();
}

FReply SAkUGCCreatorPanel::DuplicateSelected()
{
    if (Subsystem.IsValid())
    {
        FGuid EntityId;
        const FAkUGCCommandExecutionResult Result = Subsystem->DuplicateSelectedEntity(EntityId);
        Status = Result.bSucceeded
            ? FString::Printf(TEXT("Duplicated selected UGC entity as %s."), *EntityId.ToString())
            : Result.ErrorMessage;
    }
    return FReply::Handled();
}

FReply SAkUGCCreatorPanel::PlacePrefab(FName PrefabId)
{
    if (!Subsystem.IsValid())
    {
        return FReply::Handled();
    }

    FGuid EntityId;
    const FVector Location(PlacementIndex * 200.0, 0.0, 0.0);
    const FAkUGCCommandExecutionResult Result = Subsystem->PlacePrefab(PrefabId, FTransform(Location), EntityId);
    if (Result.bSucceeded)
    {
        ++PlacementIndex;
        Status = FString::Printf(TEXT("Placed %s (%s)"), *PrefabId.ToString(), *EntityId.ToString());
    }
    else
    {
        Status = Result.ErrorMessage;
    }
    return FReply::Handled();
}

FText SAkUGCCreatorPanel::GetStatusText() const
{
    if (Subsystem.IsValid() && Subsystem->GetSelectedEntityId().IsValid())
    {
        return FText::FromString(FString::Printf(
            TEXT("%s\nSelected Entity: %s"),
            *Status,
            *Subsystem->GetSelectedEntityId().ToString()));
    }
    return FText::FromString(Status);
}

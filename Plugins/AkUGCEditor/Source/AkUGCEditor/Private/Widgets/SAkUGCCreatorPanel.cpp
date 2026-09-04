#include "Widgets/SAkUGCCreatorPanel.h"

#include "Prefab/AkUGCOfficialPrefabCatalog.h"
#include "Subsystem/AkUGCEditorSubsystem.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

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
            + SVerticalBox::Slot().FillHeight(1.0f)
            [
                SNew(SSplitter)
                + SSplitter::Slot()
                .Value(0.42f)
                [
                    SNew(SVerticalBox)
                    + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
                    [SNew(STextBlock).Text(FText::FromString(TEXT("Scene Entities")))]
                    + SVerticalBox::Slot().FillHeight(0.55f)
                    [
                        SAssignNew(EntityTreeView, STreeView<TSharedPtr<FAkUGCEntityTreeItem>>)
                        .TreeItemsSource(&EntityRoots)
                        .SelectionMode(ESelectionMode::Single)
                        .OnGenerateRow(this, &SAkUGCCreatorPanel::GenerateEntityRow)
                        .OnGetChildren(this, &SAkUGCCreatorPanel::GetEntityChildren)
                        .OnSelectionChanged(this, &SAkUGCCreatorPanel::OnEntitySelectionChanged)
                    ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f, 0.0f, 4.0f)
                    [SNew(STextBlock).Text(FText::FromString(TEXT("Official Tower Defense Prefabs")))]
                    + SVerticalBox::Slot().FillHeight(0.45f)
                    [SNew(SScrollBox) + SScrollBox::Slot()[PrefabButtons]]
                ]
                + SSplitter::Slot()
                .Value(0.58f)
                [
                    SNew(SBorder)
                    .Padding(8.0f)
                    [
                        SNew(SScrollBox)
                        + SScrollBox::Slot()
                        [SAssignNew(DetailsBox, SVerticalBox)]
                    ]
                ]
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f)
            [SNew(STextBlock).Text(this, &SAkUGCCreatorPanel::GetStatusText).AutoWrapText(true)]
        ]
    ];

    RefreshFromSubsystem();
}

void SAkUGCCreatorPanel::Tick(const FGeometry& AllottedGeometry, double InCurrentTime, float InDeltaTime)
{
    SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);

    if (!Subsystem.IsValid())
    {
        return;
    }

    if (ObservedDocumentRevision != Subsystem->GetDocumentRevision())
    {
        RefreshFromSubsystem();
        return;
    }

    const FGuid CurrentSelectionId = Subsystem->GetSelectedEntityId();
    if (ObservedSelectionId != CurrentSelectionId)
    {
        ObservedSelectionId = CurrentSelectionId;
        SynchronizeTreeSelection();
        RebuildDetails();
    }
}

void SAkUGCCreatorPanel::RefreshFromSubsystem()
{
    if (!Subsystem.IsValid())
    {
        EntityRoots.Reset();
        EntityItemsById.Reset();
        return;
    }

    ObservedDocumentRevision = Subsystem->GetDocumentRevision();
    ObservedSelectionId = Subsystem->GetSelectedEntityId();
    RebuildEntityTree();
    SynchronizeTreeSelection();
    RebuildDetails();
}

void SAkUGCCreatorPanel::RebuildEntityTree()
{
    EntityRoots.Reset();
    EntityItemsById.Reset();

    if (Subsystem.IsValid() && !Subsystem->GetDocument().Scenes.IsEmpty())
    {
        const FAkUGCSceneDocument& Scene = Subsystem->GetDocument().Scenes[0];
        for (const FAkUGCEntityRecord& Entity : Scene.Entities)
        {
            TSharedPtr<FAkUGCEntityTreeItem> Item = MakeShared<FAkUGCEntityTreeItem>();
            Item->EntityId = Entity.EntityId;
            EntityItemsById.Add(Entity.EntityId, Item);
        }

        for (const FAkUGCEntityRecord& Entity : Scene.Entities)
        {
            const TSharedPtr<FAkUGCEntityTreeItem> Item = EntityItemsById.FindChecked(Entity.EntityId);
            if (Entity.ParentEntityId.IsValid())
            {
                if (const TSharedPtr<FAkUGCEntityTreeItem>* Parent = EntityItemsById.Find(Entity.ParentEntityId))
                {
                    (*Parent)->Children.Add(Item);
                    continue;
                }
            }
            EntityRoots.Add(Item);
        }
    }

    const auto SortItems = [this](TArray<TSharedPtr<FAkUGCEntityTreeItem>>& Items)
    {
        Items.Sort([this](const TSharedPtr<FAkUGCEntityTreeItem>& Left, const TSharedPtr<FAkUGCEntityTreeItem>& Right)
        {
            return GetEntityLabel(Left).ToString() < GetEntityLabel(Right).ToString();
        });
    };
    SortItems(EntityRoots);
    for (const TPair<FGuid, TSharedPtr<FAkUGCEntityTreeItem>>& Pair : EntityItemsById)
    {
        SortItems(Pair.Value->Children);
    }

    if (EntityTreeView.IsValid())
    {
        EntityTreeView->RequestTreeRefresh();
        for (const TPair<FGuid, TSharedPtr<FAkUGCEntityTreeItem>>& Pair : EntityItemsById)
        {
            EntityTreeView->SetItemExpansion(Pair.Value, true);
        }
    }
}

void SAkUGCCreatorPanel::SynchronizeTreeSelection()
{
    if (!EntityTreeView.IsValid())
    {
        return;
    }

    TGuardValue<bool> SelectionGuard(bUpdatingTreeSelection, true);
    if (const TSharedPtr<FAkUGCEntityTreeItem>* Item = EntityItemsById.Find(ObservedSelectionId))
    {
        EntityTreeView->SetSelection(*Item, ESelectInfo::Direct);
        EntityTreeView->RequestScrollIntoView(*Item);
    }
    else
    {
        EntityTreeView->ClearSelection();
    }
}

TSharedRef<ITableRow> SAkUGCCreatorPanel::GenerateEntityRow(
    TSharedPtr<FAkUGCEntityTreeItem> Item,
    const TSharedRef<STableViewBase>& OwnerTable) const
{
    return SNew(STableRow<TSharedPtr<FAkUGCEntityTreeItem>>, OwnerTable)
    [
        SNew(STextBlock)
        .Text_Lambda([this, Item]() { return GetEntityLabel(Item); })
    ];
}

void SAkUGCCreatorPanel::GetEntityChildren(
    TSharedPtr<FAkUGCEntityTreeItem> Item,
    TArray<TSharedPtr<FAkUGCEntityTreeItem>>& OutChildren) const
{
    if (Item.IsValid())
    {
        OutChildren.Append(Item->Children);
    }
}

void SAkUGCCreatorPanel::OnEntitySelectionChanged(
    TSharedPtr<FAkUGCEntityTreeItem> Item,
    ESelectInfo::Type SelectInfo)
{
    if (bUpdatingTreeSelection || !Item.IsValid() || !Subsystem.IsValid())
    {
        return;
    }

    if (!Subsystem->SelectEntity(Item->EntityId))
    {
        Status = TEXT("Unable to select the UGC entity in the level viewport.");
        SynchronizeTreeSelection();
        return;
    }

    ObservedSelectionId = Item->EntityId;
    RebuildDetails();
}

FText SAkUGCCreatorPanel::GetEntityLabel(TSharedPtr<FAkUGCEntityTreeItem> Item) const
{
    if (!Item.IsValid() || !Subsystem.IsValid())
    {
        return FText::FromString(TEXT("Invalid Entity"));
    }

    const FAkUGCEntityRecord* Entity = Subsystem->FindEntity(Item->EntityId);
    const FAkUGCPrefabDefinition* Prefab = Entity ? Subsystem->FindPrefabForEntity(Item->EntityId) : nullptr;
    const FString DisplayName = Prefab ? Prefab->DisplayName : (Entity ? Entity->PrefabId.ToString() : TEXT("Missing Entity"));
    return FText::FromString(FString::Printf(
        TEXT("%s  [%s]"),
        *DisplayName,
        *Item->EntityId.ToString(EGuidFormats::Short)));
}

void SAkUGCCreatorPanel::RebuildDetails()
{
    if (!DetailsBox.IsValid())
    {
        return;
    }

    DetailsBox->ClearChildren();
    const FAkUGCEntityRecord* Entity = Subsystem.IsValid()
        ? Subsystem->FindEntity(ObservedSelectionId)
        : nullptr;
    const FAkUGCPrefabDefinition* Prefab = Entity && Subsystem.IsValid()
        ? Subsystem->FindPrefabForEntity(Entity->EntityId)
        : nullptr;
    if (!Entity || !Prefab)
    {
        DetailsBox->AddSlot().AutoHeight()
        [SNew(STextBlock).Text(FText::FromString(TEXT("Select a UGC entity to edit its properties.")))];
        return;
    }

    DetailsBox->AddSlot().AutoHeight()
    [SNew(STextBlock).Text(FText::FromString(Prefab->DisplayName))];
    DetailsBox->AddSlot().AutoHeight().Padding(0.0f, 2.0f)
    [SNew(STextBlock).Text(FText::FromString(Entity->PrefabId.ToString()))];
    DetailsBox->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
    [SNew(STextBlock).Text(FText::FromString(Entity->EntityId.ToString()))];
    DetailsBox->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
    [SNew(SSeparator)];
    DetailsBox->AddSlot().AutoHeight().Padding(0.0f, 3.0f)
    [
        SNew(SHorizontalBox)
        + SHorizontalBox::Slot().FillWidth(0.42f).VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
        [SNew(STextBlock).Text(FText::FromString(TEXT("Parent")))]
        + SHorizontalBox::Slot().FillWidth(0.58f)
        [
            SNew(SComboButton)
            .OnGetMenuContent_Lambda([this, EntityId = Entity->EntityId]()
            {
                return BuildParentMenu(EntityId);
            })
            .ButtonContent()
            [
                SNew(STextBlock)
                .Text_Lambda([this, EntityId = Entity->EntityId]()
                {
                    return GetParentLabel(EntityId);
                })
            ]
        ]
    ];
    DetailsBox->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
    [SNew(SSeparator)];

    if (Prefab->EditableProperties.IsEmpty())
    {
        DetailsBox->AddSlot().AutoHeight()
        [SNew(STextBlock).Text(FText::FromString(TEXT("This prefab has no editable properties.")))];
        return;
    }

    for (const FAkUGCPropertyDefinition& Property : Prefab->EditableProperties)
    {
        const FAkUGCValue* StoredValue = FindPropertyValue(*Entity, Property);
        const FAkUGCValue& Value = StoredValue ? *StoredValue : Property.DefaultValue;

        DetailsBox->AddSlot().AutoHeight().Padding(0.0f, 3.0f)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(0.42f).VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
            [
                SNew(STextBlock)
                .Text(FText::FromString(FString::Printf(
                    TEXT("%s.%s"),
                    *Property.ComponentTypeId.ToString(),
                    *Property.PropertyId.ToString())))
            ]
            + SHorizontalBox::Slot().FillWidth(0.58f)
            [BuildPropertyEditor(Entity->EntityId, Property, Value)]
        ];
    }
}

FText SAkUGCCreatorPanel::GetParentLabel(const FGuid& EntityId) const
{
    if (!Subsystem.IsValid())
    {
        return FText::FromString(TEXT("Scene Root"));
    }

    const FAkUGCEntityRecord* Entity = Subsystem->FindEntity(EntityId);
    if (!Entity || !Entity->ParentEntityId.IsValid())
    {
        return FText::FromString(TEXT("Scene Root"));
    }

    const FAkUGCEntityRecord* Parent = Subsystem->FindEntity(Entity->ParentEntityId);
    const FAkUGCPrefabDefinition* ParentPrefab = Parent
        ? Subsystem->FindPrefabForEntity(Parent->EntityId)
        : nullptr;
    const FString ParentName = ParentPrefab
        ? ParentPrefab->DisplayName
        : (Parent ? Parent->PrefabId.ToString() : TEXT("Missing Parent"));
    return FText::FromString(FString::Printf(
        TEXT("%s  [%s]"),
        *ParentName,
        *Entity->ParentEntityId.ToString(EGuidFormats::Short)));
}

TSharedRef<SWidget> SAkUGCCreatorPanel::BuildParentMenu(const FGuid& EntityId)
{
    FMenuBuilder MenuBuilder(true, nullptr);
    const TWeakPtr<SAkUGCCreatorPanel> WeakThis = SharedThis(this);
    const FAkUGCEntityRecord* Entity = Subsystem.IsValid() ? Subsystem->FindEntity(EntityId) : nullptr;
    if (Entity && Entity->ParentEntityId.IsValid())
    {
        MenuBuilder.AddMenuEntry(
            FText::FromString(TEXT("Scene Root")),
            FText::FromString(TEXT("Detach this entity from its current parent.")),
            FSlateIcon(),
            FUIAction(FExecuteAction::CreateLambda([WeakThis, EntityId]()
            {
                if (const TSharedPtr<SAkUGCCreatorPanel> Panel = WeakThis.Pin())
                {
                    Panel->CommitParent(EntityId, FGuid{});
                }
            })));
    }

    if (!Subsystem.IsValid() || Subsystem->GetDocument().Scenes.IsEmpty())
    {
        return MenuBuilder.MakeWidget();
    }

    TArray<FGuid> CandidateIds;
    for (const FAkUGCEntityRecord& Candidate : Subsystem->GetDocument().Scenes[0].Entities)
    {
        if (IsValidParentCandidate(EntityId, Candidate.EntityId)
            && (!Entity || Candidate.EntityId != Entity->ParentEntityId))
        {
            CandidateIds.Add(Candidate.EntityId);
        }
    }
    CandidateIds.Sort([this](const FGuid& Left, const FGuid& Right)
    {
        const TSharedPtr<FAkUGCEntityTreeItem>* LeftItem = EntityItemsById.Find(Left);
        const TSharedPtr<FAkUGCEntityTreeItem>* RightItem = EntityItemsById.Find(Right);
        const FString LeftLabel = LeftItem ? GetEntityLabel(*LeftItem).ToString() : Left.ToString();
        const FString RightLabel = RightItem ? GetEntityLabel(*RightItem).ToString() : Right.ToString();
        return LeftLabel < RightLabel;
    });

    MenuBuilder.BeginSection(TEXT("EntityParents"), FText::FromString(TEXT("Entities")));
    for (const FGuid& CandidateId : CandidateIds)
    {
        const TSharedPtr<FAkUGCEntityTreeItem>* Item = EntityItemsById.Find(CandidateId);
        const FText Label = Item ? GetEntityLabel(*Item) : FText::FromString(CandidateId.ToString());
        MenuBuilder.AddMenuEntry(
            Label,
            FText::FromString(TEXT("Attach this entity while preserving its world transform.")),
            FSlateIcon(),
            FUIAction(FExecuteAction::CreateLambda([WeakThis, EntityId, CandidateId]()
            {
                if (const TSharedPtr<SAkUGCCreatorPanel> Panel = WeakThis.Pin())
                {
                    Panel->CommitParent(EntityId, CandidateId);
                }
            })));
    }
    MenuBuilder.EndSection();
    return MenuBuilder.MakeWidget();
}

void SAkUGCCreatorPanel::CommitParent(const FGuid& EntityId, const FGuid& ParentEntityId)
{
    if (!Subsystem.IsValid())
    {
        return;
    }

    const FAkUGCCommandExecutionResult Result = Subsystem->SetEntityParent(EntityId, ParentEntityId);
    Status = Result.bSucceeded
        ? (ParentEntityId.IsValid() ? TEXT("Updated entity parent.") : TEXT("Moved entity to scene root."))
        : Result.ErrorMessage;
    if (Result.bSucceeded)
    {
        RefreshFromSubsystem();
    }
}

bool SAkUGCCreatorPanel::IsValidParentCandidate(
    const FGuid& EntityId,
    const FGuid& CandidateParentId) const
{
    if (!Subsystem.IsValid() || !CandidateParentId.IsValid() || CandidateParentId == EntityId)
    {
        return false;
    }

    FGuid AncestorId = CandidateParentId;
    while (AncestorId.IsValid())
    {
        if (AncestorId == EntityId)
        {
            return false;
        }
        const FAkUGCEntityRecord* Ancestor = Subsystem->FindEntity(AncestorId);
        if (!Ancestor)
        {
            return false;
        }
        AncestorId = Ancestor->ParentEntityId;
    }
    return true;
}

TSharedRef<SWidget> SAkUGCCreatorPanel::BuildPropertyEditor(
    const FGuid& EntityId,
    const FAkUGCPropertyDefinition& Property,
    const FAkUGCValue& Value)
{
    switch (Property.ValueType)
    {
    case EAkUGCValueType::Bool:
        return SNew(SCheckBox)
            .IsChecked(Value.BoolValue ? ECheckBoxState::Checked : ECheckBoxState::Unchecked)
            .OnCheckStateChanged_Lambda([this, EntityId, Property](ECheckBoxState State)
            {
                FAkUGCValue NewValue;
                NewValue.Type = EAkUGCValueType::Bool;
                NewValue.BoolValue = State == ECheckBoxState::Checked;
                CommitProperty(EntityId, Property, NewValue);
            });

    case EAkUGCValueType::Integer:
        return SNew(SNumericEntryBox<int64>)
            .Value(Value.IntegerValue)
            .MinValue(Property.bHasMinimum ? TOptional<int64>(static_cast<int64>(Property.Minimum)) : TOptional<int64>())
            .MaxValue(Property.bHasMaximum ? TOptional<int64>(static_cast<int64>(Property.Maximum)) : TOptional<int64>())
            .OnValueCommitted_Lambda([this, EntityId, Property](int64 NewInteger, ETextCommit::Type)
            {
                FAkUGCValue NewValue;
                NewValue.Type = EAkUGCValueType::Integer;
                NewValue.IntegerValue = NewInteger;
                CommitProperty(EntityId, Property, NewValue);
            });

    case EAkUGCValueType::Number:
        return SNew(SNumericEntryBox<double>)
            .Value(Value.NumberValue)
            .MinValue(Property.bHasMinimum ? TOptional<double>(Property.Minimum) : TOptional<double>())
            .MaxValue(Property.bHasMaximum ? TOptional<double>(Property.Maximum) : TOptional<double>())
            .OnValueCommitted_Lambda([this, EntityId, Property](double NewNumber, ETextCommit::Type)
            {
                FAkUGCValue NewValue;
                NewValue.Type = EAkUGCValueType::Number;
                NewValue.NumberValue = NewNumber;
                CommitProperty(EntityId, Property, NewValue);
            });

    case EAkUGCValueType::String:
        return SNew(SEditableTextBox)
            .Text(FText::FromString(Value.StringValue))
            .OnTextCommitted_Lambda([this, EntityId, Property](const FText& Text, ETextCommit::Type)
            {
                FAkUGCValue NewValue;
                NewValue.Type = EAkUGCValueType::String;
                NewValue.StringValue = Text.ToString();
                CommitProperty(EntityId, Property, NewValue);
            });

    case EAkUGCValueType::Name:
        return SNew(SEditableTextBox)
            .Text(FText::FromName(Value.NameValue))
            .OnTextCommitted_Lambda([this, EntityId, Property](const FText& Text, ETextCommit::Type)
            {
                FAkUGCValue NewValue;
                NewValue.Type = EAkUGCValueType::Name;
                NewValue.NameValue = FName(*Text.ToString());
                CommitProperty(EntityId, Property, NewValue);
            });

    case EAkUGCValueType::Vector:
        return SNew(SEditableTextBox)
            .Text(FText::FromString(Value.VectorValue.ToString()))
            .OnTextCommitted_Lambda([this, EntityId, Property](const FText& Text, ETextCommit::Type)
            {
                FAkUGCValue NewValue;
                NewValue.Type = EAkUGCValueType::Vector;
                if (!NewValue.VectorValue.InitFromString(Text.ToString()))
                {
                    Status = TEXT("Vector must use the format X=0 Y=0 Z=0.");
                    return;
                }
                CommitProperty(EntityId, Property, NewValue);
            });

    case EAkUGCValueType::Rotator:
        return SNew(SEditableTextBox)
            .Text(FText::FromString(Value.RotatorValue.ToString()))
            .OnTextCommitted_Lambda([this, EntityId, Property](const FText& Text, ETextCommit::Type)
            {
                FAkUGCValue NewValue;
                NewValue.Type = EAkUGCValueType::Rotator;
                if (!NewValue.RotatorValue.InitFromString(Text.ToString()))
                {
                    Status = TEXT("Rotator must use the format P=0 Y=0 R=0.");
                    return;
                }
                CommitProperty(EntityId, Property, NewValue);
            });
    }

    return SNew(STextBlock).Text(FText::FromString(TEXT("Unsupported property type")));
}

void SAkUGCCreatorPanel::CommitProperty(
    const FGuid& EntityId,
    const FAkUGCPropertyDefinition& Property,
    const FAkUGCValue& Value)
{
    if (!Subsystem.IsValid())
    {
        return;
    }

    const FAkUGCCommandExecutionResult Result = Subsystem->SetEntityProperty(
        EntityId,
        Property.ComponentTypeId,
        Property.PropertyId,
        Value);
    Status = Result.bSucceeded
        ? FString::Printf(TEXT("Updated %s.%s."), *Property.ComponentTypeId.ToString(), *Property.PropertyId.ToString())
        : Result.ErrorMessage;
    if (Result.bSucceeded)
    {
        RefreshFromSubsystem();
    }
}

const FAkUGCValue* SAkUGCCreatorPanel::FindPropertyValue(
    const FAkUGCEntityRecord& Entity,
    const FAkUGCPropertyDefinition& Property)
{
    const FAkUGCComponentRecord* Component = Entity.Components.FindByPredicate(
        [&Property](const FAkUGCComponentRecord& Candidate)
        {
            return Candidate.TypeId == Property.ComponentTypeId;
        });
    return Component ? Component->Properties.Find(Property.PropertyId) : nullptr;
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

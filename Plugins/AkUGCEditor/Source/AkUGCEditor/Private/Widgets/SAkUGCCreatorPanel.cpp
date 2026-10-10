#include "Widgets/SAkUGCCreatorPanel.h"

#include "Prefab/AkUGCOfficialPrefabCatalog.h"
#include "Subsystem/AkUGCEditorSubsystem.h"
#include "Widgets/SAkUGCLogicCanvas.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

void SAkUGCTowerDefenseParameterForm::Construct(const FArguments& InArgs, UAkUGCEditorSubsystem* InSubsystem)
{
    Subsystem = InSubsystem;
    ChildSlot
    [
        SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f)
        [SNew(STextBlock).Text(FText::FromString(TEXT("塔防简单参数 · 无需编辑 Logic")))]
        + SVerticalBox::Slot().AutoHeight()
        [SNew(STextBlock).Text(FText::FromString(TEXT("新建即包含可玩场景。修改先保留在表单中，点击应用一次写入项目；Undo / Redo 会重新读取当前项目。"))).AutoWrapText(true)]
        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f)
        [SAssignNew(FieldsBox, SVerticalBox)]
        + SVerticalBox::Slot().AutoHeight()
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().Padding(2.0f)
            [SNew(SButton).Text(FText::FromString(TEXT("应用参数（一次撤销）")))
                .IsEnabled_Lambda([this]() { return bReadable && bDirty; })
                .OnClicked(this, &SAkUGCTowerDefenseParameterForm::ApplyParameters)]
            + SHorizontalBox::Slot().AutoWidth().Padding(2.0f)
            [SNew(SButton).Text(FText::FromString(TEXT("重新读取项目 / 放弃草稿")))
                .IsEnabled_Lambda([this]() { return Subsystem.IsValid() && Subsystem->HasOpenProject(); })
                .OnClicked(this, &SAkUGCTowerDefenseParameterForm::ResetParameters)]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f)
        [SNew(STextBlock).Text_Lambda([this]() { return FText::FromString(Status); }).AutoWrapText(true)]
    ];
    RefreshParameters();
}

void SAkUGCTowerDefenseParameterForm::Tick(const FGeometry& AllottedGeometry, double InCurrentTime, float InDeltaTime)
{
    SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
    if (Subsystem.IsValid() && ObservedDocumentRevision != Subsystem->GetDocumentRevision())
    {
        const bool bHadDraft = bDirty;
        RefreshParameters();
        if (bReadable && bHadDraft)
        {
            Status = TEXT("项目已变化（包括 Undo / Redo）；已重新读取当前参数，旧草稿已放弃。");
        }
    }
    else if (bRebuildRequested)
    {
        RebuildFields();
    }
}

void SAkUGCTowerDefenseParameterForm::RefreshParameters()
{
    bReadable = false;
    bDirty = false;
    Status = TEXT("请先新建可玩塔防项目，或加载已有塔防项目。");
    if (Subsystem.IsValid())
    {
        ObservedDocumentRevision = Subsystem->GetDocumentRevision();
        if (Subsystem->HasOpenProject())
        {
            FString Error;
            bReadable = Subsystem->GetTowerDefenseParameters(Draft, &Error);
            Status = bReadable ? TEXT("已读取当前项目参数。修改后请显式应用；保存不会自动应用草稿。")
                : FString::Printf(TEXT("无法读取简单参数：%s。项目未修改，可使用高级编辑修复。"), *Error);
        }
    }
    RebuildFields();
}

void SAkUGCTowerDefenseParameterForm::AddNumberField(
    const TCHAR* Label, double Minimum, double Maximum,
    TFunction<double()> ReadValue, TFunction<void(double)> WriteValue)
{
    FieldsBox->AddSlot().AutoHeight().Padding(0.0f, 2.0f)
    [
        SNew(SHorizontalBox)
        + SHorizontalBox::Slot().FillWidth(0.45f).VAlign(VAlign_Center)
        [SNew(STextBlock).Text(FText::FromString(Label))]
        + SHorizontalBox::Slot().FillWidth(0.55f)
        [
            SNew(SNumericEntryBox<double>)
            .MinValue(Minimum).MaxValue(Maximum).MinSliderValue(Minimum).MaxSliderValue(Maximum)
            .AllowSpin(true)
            .Value_Lambda([ReadValue]() { return TOptional<double>(ReadValue()); })
            .OnValueChanged_Lambda([this, WriteValue](double Value) { WriteValue(Value); bDirty = true; })
            .OnValueCommitted_Lambda([this, WriteValue](double Value, ETextCommit::Type) { WriteValue(Value); bDirty = true; })
        ]
    ];
}

void SAkUGCTowerDefenseParameterForm::AddIntegerField(
    const TCHAR* Label, int32 Minimum, int32 Maximum,
    TFunction<int32()> ReadValue, TFunction<void(int32)> WriteValue)
{
    FieldsBox->AddSlot().AutoHeight().Padding(0.0f, 2.0f)
    [
        SNew(SHorizontalBox)
        + SHorizontalBox::Slot().FillWidth(0.45f).VAlign(VAlign_Center)
        [SNew(STextBlock).Text(FText::FromString(Label))]
        + SHorizontalBox::Slot().FillWidth(0.55f)
        [
            SNew(SNumericEntryBox<int32>)
            .MinValue(Minimum).MaxValue(Maximum).MinSliderValue(Minimum).MaxSliderValue(Maximum)
            .AllowSpin(true)
            .Value_Lambda([ReadValue]() { return TOptional<int32>(ReadValue()); })
            .OnValueChanged_Lambda([this, WriteValue](int32 Value) { WriteValue(Value); bDirty = true; })
            .OnValueCommitted_Lambda([this, WriteValue](int32 Value, ETextCommit::Type) { WriteValue(Value); bDirty = true; })
        ]
    ];
}

void SAkUGCTowerDefenseParameterForm::RebuildFields()
{
    bRebuildRequested = false;
    FieldsBox->ClearChildren();
    if (!bReadable)
    {
        return;
    }
    AddIntegerField(TEXT("波数（1–3，默认 3）"), 1, 3,
        [this]() { return Draft.Waves.Num(); }, [this](int32 Value)
        {
            const int32 Count = FMath::Clamp(Value, 1, 3);
            if (Count != Draft.Waves.Num())
            {
                Draft.Waves.SetNum(Count);
                bRebuildRequested = true;
            }
        });
    AddNumberField(TEXT("基地生命"), 1.0, 100000.0,
        [this]() { return Draft.BaseMaxHealth; }, [this](double Value) { Draft.BaseMaxHealth = Value; });
    AddIntegerField(TEXT("初始资源"), 0, AkUGCTowerDefenseRulesetLimits::MaxInitialResources,
        [this]() { return Draft.InitialResources; }, [this](int32 Value) { Draft.InitialResources = Value; });
    AddNumberField(TEXT("波间隔（秒）"), 0.0, AkUGCTowerDefenseRulesetLimits::MaxWaveIntervalSeconds,
        [this]() { return Draft.WaveIntervalSeconds; }, [this](double Value) { Draft.WaveIntervalSeconds = Value; });
    FieldsBox->AddSlot().AutoHeight().Padding(0.0f, 4.0f)
    [SNew(STextBlock).Text(FText::FromString(TEXT("失败规则：基地生命耗尽（BaseHealthDepleted，当前唯一规则）\n胜利规则：全部波次敌人清除（AllWavesCleared，当前唯一规则）"))).AutoWrapText(true)];
    for (int32 WaveIndex = 0; WaveIndex < Draft.Waves.Num(); ++WaveIndex)
    {
        FieldsBox->AddSlot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 4.0f)
        [SNew(STextBlock).Text(FText::FromString(FString::Printf(TEXT("第 %d 波"), WaveIndex + 1)))];
        FieldsBox->AddSlot().AutoHeight().Padding(0.0f, 2.0f)
        [SNew(STextBlock).Text(FText::FromString(FString::Printf(TEXT("官方敌人：基础敌人（%s，白名单只读）"), *Draft.Waves[WaveIndex].EnemyPrefabId.ToString()))).AutoWrapText(true)];
        AddIntegerField(TEXT("敌人数量（各波总计 ≤ 500）"), 1, 500,
            [this, WaveIndex]() { return Draft.Waves.IsValidIndex(WaveIndex) ? Draft.Waves[WaveIndex].EnemyCount : 1; },
            [this, WaveIndex](int32 Value) { if (Draft.Waves.IsValidIndex(WaveIndex)) Draft.Waves[WaveIndex].EnemyCount = Value; });
        const auto AddWaveNumber = [this, WaveIndex](const TCHAR* Label, double Minimum, double Maximum,
            double FAkUGCTowerDefenseWaveParameters::* Member)
        {
            AddNumberField(Label, Minimum, Maximum,
                [this, WaveIndex, Member]() { return Draft.Waves.IsValidIndex(WaveIndex) ? Draft.Waves[WaveIndex].*Member : 0.0; },
                [this, WaveIndex, Member](double Value) { if (Draft.Waves.IsValidIndex(WaveIndex)) Draft.Waves[WaveIndex].*Member = Value; });
        };
        AddWaveNumber(TEXT("刷怪间隔（秒）"), 0.1, 60.0, &FAkUGCTowerDefenseWaveParameters::SpawnIntervalSeconds);
        AddWaveNumber(TEXT("起始延迟（秒）"), 0.0, AkUGCTowerDefenseRulesetLimits::MaxStartDelaySeconds, &FAkUGCTowerDefenseWaveParameters::StartDelaySeconds);
        AddWaveNumber(TEXT("敌人生命"), 1.0, 100000.0, &FAkUGCTowerDefenseWaveParameters::EnemyMaxHealth);
        AddWaveNumber(TEXT("移动速度（厘米 / 秒）"), 10.0, 2000.0, &FAkUGCTowerDefenseWaveParameters::EnemyMoveSpeed);
        AddWaveNumber(TEXT("到达基地伤害"), 0.0, 100000.0, &FAkUGCTowerDefenseWaveParameters::EnemyGoalDamage);
    }
}

FReply SAkUGCTowerDefenseParameterForm::ApplyParameters()
{
    if (!Subsystem.IsValid() || !bReadable)
    {
        return FReply::Handled();
    }
    if (ObservedDocumentRevision != Subsystem->GetDocumentRevision())
    {
        RefreshParameters();
        Status = TEXT("项目已变化；已读取最新参数，请确认后重新修改并应用。");
        return FReply::Handled();
    }
    FString Error;
    if (!Subsystem->ApplyTowerDefenseParameters(Draft, &Error))
    {
        Status = FString::Printf(TEXT("应用失败，项目未修改：%s"), *Error);
    }
    else
    {
        RefreshParameters();
        Status = TEXT("全部参数已作为一次事务应用。可一次 Undo / Redo，保存会保留这些参数。");
    }
    return FReply::Handled();
}

FReply SAkUGCTowerDefenseParameterForm::ResetParameters()
{
    RefreshParameters();
    return FReply::Handled();
}

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
                .Text(FText::FromString(TEXT("AkUGC Creator Studio · 塔防创作")))
            ]
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.0f, 6.0f)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth().Padding(2.0f)
                [SNew(SButton).Text(FText::FromString(TEXT("新建默认可玩塔防"))).OnClicked(this, &SAkUGCCreatorPanel::NewProject)]
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
                SNew(SScrollBox)
                + SScrollBox::Slot()
                [
                    SNew(SVerticalBox)
                    + SVerticalBox::Slot().AutoHeight()
                    [SNew(SAkUGCTowerDefenseParameterForm, Subsystem.Get())]
                    + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f)
                    [
                        SNew(SExpandableArea)
                        .InitiallyCollapsed(true)
                        .HeaderContent()
                        [SNew(STextBlock).Text(FText::FromString(TEXT("高级编辑：场景 / 实体属性 / Logic（可选）")))]
                        .BodyContent()
                        [
                            SNew(SBox).HeightOverride(620.0f)
                            [
                SNew(SSplitter)
                + SSplitter::Slot()
                .Value(0.42f)
                [
                    SNew(SVerticalBox)
                    + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
                    [SNew(STextBlock).Text(FText::FromString(TEXT("Scene Entities")))]
                    + SVerticalBox::Slot().FillHeight(0.30f)
                    [
                        SAssignNew(EntityTreeView, STreeView<TSharedPtr<FAkUGCEntityTreeItem>>)
                        .TreeItemsSource(&EntityRoots)
                        .SelectionMode(ESelectionMode::Single)
                        .OnGenerateRow(this, &SAkUGCCreatorPanel::GenerateEntityRow)
                        .OnGetChildren(this, &SAkUGCCreatorPanel::GetEntityChildren)
                        .OnSelectionChanged(this, &SAkUGCCreatorPanel::OnEntitySelectionChanged)
                    ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f, 0.0f, 4.0f)
                    [
                        SNew(SHorizontalBox)
                        + SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
                        [SNew(STextBlock).Text(FText::FromString(TEXT("Logic Nodes")))]
                        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                        [
                            SNew(SButton)
                            .Text_Lambda([this]() { return GetLogicViewToggleLabel(); })
                            .OnClicked(this, &SAkUGCCreatorPanel::ToggleLogicGraphView)
                        ]
                    ]
                    + SVerticalBox::Slot().FillHeight(0.30f)
                    [
                        SAssignNew(LogicViewSwitcher, SWidgetSwitcher)
                        + SWidgetSwitcher::Slot()
                        [
                            SNew(SVerticalBox)
                            + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
                            [
                                SNew(SHorizontalBox)
                                + SHorizontalBox::Slot().AutoWidth().Padding(2.0f)
                                [
                                    SNew(SComboButton)
                                    .OnGetMenuContent(this, &SAkUGCCreatorPanel::BuildAddLogicNodeMenu)
                                    .ButtonContent()
                                    [SNew(STextBlock).Text(FText::FromString(TEXT("Add Logic Node")))]
                                ]
                                + SHorizontalBox::Slot().AutoWidth().Padding(2.0f)
                                [SNew(SButton).Text(FText::FromString(TEXT("Delete Node"))).OnClicked(this, &SAkUGCCreatorPanel::DeleteSelectedLogicNode)]
                            ]
                            + SVerticalBox::Slot().FillHeight(1.0f)
                            [
                                SAssignNew(LogicListView, SListView<TSharedPtr<FAkUGCLogicNode>>)
                                .ListItemsSource(&LogicItems)
                                .SelectionMode(ESelectionMode::Single)
                                .OnGenerateRow(this, &SAkUGCCreatorPanel::GenerateLogicRow)
                                .OnSelectionChanged(this, &SAkUGCCreatorPanel::OnLogicSelectionChanged)
                            ]
                        ]
                        + SWidgetSwitcher::Slot()
                        [
                            SAssignNew(LogicCanvas, SAkUGCLogicCanvas, Subsystem.Get())
                        ]
                    ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f, 0.0f, 4.0f)
                    [SNew(STextBlock).Text(FText::FromString(TEXT("Logic Connections")))]
                    + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
                    [
                        SNew(SHorizontalBox)
                        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 2.0f, 0.0f)
                        [
                            SNew(SComboButton)
                            .OnGetMenuContent(this, &SAkUGCCreatorPanel::BuildSourceNodeMenu)
                            .ButtonContent()
                            [SNew(STextBlock).Text_Lambda([this]() { return GetPendingSourceLabel(); })]
                        ]
                        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 2.0f, 0.0f)
                        [SNew(STextBlock).Text(FText::FromString(TEXT("\x2192")))]
                        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 2.0f, 0.0f)
                        [
                            SNew(SComboButton)
                            .OnGetMenuContent(this, &SAkUGCCreatorPanel::BuildTargetNodeMenu)
                            .ButtonContent()
                            [SNew(STextBlock).Text_Lambda([this]() { return GetPendingTargetLabel(); })]
                        ]
                        + SHorizontalBox::Slot().AutoWidth().Padding(2.0f)
                        [SNew(SButton).Text(FText::FromString(TEXT("Connect"))).OnClicked(this, &SAkUGCCreatorPanel::ConnectPendingNodes)]
                    ]
                    + SVerticalBox::Slot().FillHeight(0.12f)
                    [SNew(SScrollBox) + SScrollBox::Slot()[SAssignNew(ConnectionsBox, SVerticalBox)]]
                    + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f)
                    [SNew(STextBlock).Text_Lambda([this]() { return GetLogicValidationText(); }).AutoWrapText(true)]
                    + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f, 0.0f, 4.0f)
                    [SNew(STextBlock).Text(FText::FromString(TEXT("Official Tower Defense Prefabs")))]
                    + SVerticalBox::Slot().FillHeight(0.24f)
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
                        ]
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
        LogicItems.Reset();
        SelectedLogicNodeId.Invalidate();
        return;
    }

    ObservedDocumentRevision = Subsystem->GetDocumentRevision();
    ObservedSelectionId = Subsystem->GetSelectedEntityId();
    RebuildEntityTree();
    SynchronizeTreeSelection();
    RebuildLogicList();
    RebuildConnections();
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

    SelectedLogicNodeId.Invalidate();
    if (LogicListView.IsValid())
    {
        LogicListView->ClearSelection();
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
    if (SelectedLogicNodeId.IsValid())
    {
        RebuildLogicDetails();
        return;
    }

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
        ? TEXT("已新建默认可玩塔防（三波）。直接修改简单参数即可；高级 Logic 无需操作。")
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
    if (SelectedLogicNodeId.IsValid())
    {
        return FText::FromString(FString::Printf(
            TEXT("%s\nEditing Logic Node: %s"),
            *Status,
            *SelectedLogicNodeId.ToString()));
    }
    if (Subsystem.IsValid() && Subsystem->GetSelectedEntityId().IsValid())
    {
        return FText::FromString(FString::Printf(
            TEXT("%s\nSelected Entity: %s"),
            *Status,
            *Subsystem->GetSelectedEntityId().ToString()));
    }
    return FText::FromString(Status);
}

void SAkUGCCreatorPanel::RebuildLogicList()
{
    LogicItems.Reset();

    const FAkUGCLogicGraph* Graph = Subsystem.IsValid() ? Subsystem->GetLogicGraph() : nullptr;
    if (Graph)
    {
        for (const FAkUGCLogicNode& Node : Graph->Nodes)
        {
            LogicItems.Add(MakeShared<FAkUGCLogicNode>(Node));
        }
    }

    LogicItems.Sort([this](const TSharedPtr<FAkUGCLogicNode>& Left, const TSharedPtr<FAkUGCLogicNode>& Right)
    {
        if (Left->Type != Right->Type)
        {
            return static_cast<uint8>(Left->Type) < static_cast<uint8>(Right->Type);
        }
        return Left->NodeId.ToString() < Right->NodeId.ToString();
    });

    if (SelectedLogicNodeId.IsValid() && !FindLogicNode(SelectedLogicNodeId))
    {
        SelectedLogicNodeId.Invalidate();
    }

    if (LogicListView.IsValid())
    {
        LogicListView->RequestListRefresh();
        if (SelectedLogicNodeId.IsValid())
        {
            const TSharedPtr<FAkUGCLogicNode>* Selected = LogicItems.FindByPredicate(
                [this](const TSharedPtr<FAkUGCLogicNode>& Item)
                {
                    return Item.IsValid() && Item->NodeId == SelectedLogicNodeId;
                });
            if (Selected)
            {
                TGuardValue<bool> Guard(bUpdatingLogicSelection, true);
                LogicListView->SetSelection(*Selected, ESelectInfo::Direct);
            }
        }
    }
}

void SAkUGCCreatorPanel::RebuildConnections()
{
    if (!ConnectionsBox.IsValid())
    {
        return;
    }

    ConnectionsBox->ClearChildren();
    const FAkUGCLogicGraph* Graph = Subsystem.IsValid() ? Subsystem->GetLogicGraph() : nullptr;
    if (!Graph || Graph->Connections.IsEmpty())
    {
        ConnectionsBox->AddSlot().AutoHeight()
        [SNew(STextBlock).Text(FText::FromString(TEXT("No connections.")))];
        return;
    }

    for (const FAkUGCLogicConnection& Connection : Graph->Connections)
    {
        const FAkUGCLogicNode* Source = FindLogicNode(Connection.SourceNodeId);
        const FAkUGCLogicNode* Target = FindLogicNode(Connection.TargetNodeId);
        if (!Source || !Target)
        {
            continue;
        }

        const FGuid SourceId = Connection.SourceNodeId;
        const FGuid TargetId = Connection.TargetNodeId;
        ConnectionsBox->AddSlot().AutoHeight().Padding(0.0f, 1.0f)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
            [SNew(STextBlock).Text(FText::FromString(FString::Printf(
                TEXT("%s \x2192 %s"),
                *GetLogicNodeLabel(*Source).ToString(),
                *GetLogicNodeLabel(*Target).ToString())))]
            + SHorizontalBox::Slot().AutoWidth().Padding(4.0f, 0.0f, 0.0f, 0.0f)
            [
                SNew(SButton)
                .Text(FText::FromString(TEXT("X")))
                .OnClicked_Lambda([this, SourceId, TargetId]()
                {
                    RemoveLogicConnection(SourceId, TargetId);
                    return FReply::Handled();
                })
            ]
        ];
    }
}

TSharedRef<ITableRow> SAkUGCCreatorPanel::GenerateLogicRow(
    TSharedPtr<FAkUGCLogicNode> Item,
    const TSharedRef<STableViewBase>& OwnerTable) const
{
    return SNew(STableRow<TSharedPtr<FAkUGCLogicNode>>, OwnerTable)
    [
        SNew(STextBlock)
        .Text_Lambda([this, Item]()
        {
            return Item.IsValid() ? GetLogicNodeLabel(*Item) : FText::GetEmpty();
        })
    ];
}

void SAkUGCCreatorPanel::OnLogicSelectionChanged(TSharedPtr<FAkUGCLogicNode> Item, ESelectInfo::Type SelectInfo)
{
    if (bUpdatingLogicSelection)
    {
        return;
    }

    if (!Item.IsValid())
    {
        SelectedLogicNodeId.Invalidate();
    }
    else
    {
        SelectedLogicNodeId = Item->NodeId;
        if (EntityTreeView.IsValid())
        {
            TGuardValue<bool> Guard(bUpdatingTreeSelection, true);
            EntityTreeView->ClearSelection();
        }
    }

    RebuildDetails();
}

FText SAkUGCCreatorPanel::GetLogicNodeLabel(const FAkUGCLogicNode& Node) const
{
    const FText TypeName = GetLogicNodeTypeDisplayName(Node.Type);
    switch (Node.Type)
    {
    case EAkUGCLogicNodeType::Message:
        return FText::FromString(FString::Printf(TEXT("%s: \"%s\""), *TypeName.ToString(), *Node.Message));
    case EAkUGCLogicNodeType::Timer:
        return FText::FromString(FString::Printf(TEXT("%s: %.1fs"), *TypeName.ToString(), Node.DelaySeconds));
    case EAkUGCLogicNodeType::Spawn:
        return FText::FromString(FString::Printf(TEXT("%s: %s"), *TypeName.ToString(), *Node.SpawnPrefabId.ToString()));
    default:
        return TypeName;
    }
}

FText SAkUGCCreatorPanel::GetLogicNodeTypeDisplayName(EAkUGCLogicNodeType Type)
{
    switch (Type)
    {
    case EAkUGCLogicNodeType::GameStart: return FText::FromString(TEXT("Game Start"));
    case EAkUGCLogicNodeType::WaveStart: return FText::FromString(TEXT("Wave Start"));
    case EAkUGCLogicNodeType::Message: return FText::FromString(TEXT("Message"));
    case EAkUGCLogicNodeType::Timer: return FText::FromString(TEXT("Timer"));
    case EAkUGCLogicNodeType::Spawn: return FText::FromString(TEXT("Spawn"));
    }
    return FText::FromString(TEXT("Unknown"));
}

void SAkUGCCreatorPanel::RebuildLogicDetails()
{
    if (!DetailsBox.IsValid())
    {
        return;
    }

    DetailsBox->ClearChildren();
    const FAkUGCLogicNode* Node = FindLogicNode(SelectedLogicNodeId);
    if (!Node)
    {
        DetailsBox->AddSlot().AutoHeight()
        [SNew(STextBlock).Text(FText::FromString(TEXT("The selected logic node no longer exists.")))];
        return;
    }

    DetailsBox->AddSlot().AutoHeight()
    [SNew(STextBlock).Text(GetLogicNodeLabel(*Node))];
    DetailsBox->AddSlot().AutoHeight().Padding(0.0f, 2.0f)
    [SNew(STextBlock).Text(FText::FromString(Node->NodeId.ToString()))];
    DetailsBox->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
    [SNew(SSeparator)];

    switch (Node->Type)
    {
    case EAkUGCLogicNodeType::GameStart:
        DetailsBox->AddSlot().AutoHeight().Padding(0.0f, 3.0f)
        [SNew(STextBlock).Text(FText::FromString(TEXT("Fires when the match begins. No parameters.")))];
        break;

    case EAkUGCLogicNodeType::WaveStart:
        DetailsBox->AddSlot().AutoHeight().Padding(0.0f, 3.0f)
        [SNew(STextBlock).Text(FText::FromString(TEXT("Fires when a wave starts. No parameters.")))];
        break;

    case EAkUGCLogicNodeType::Message:
        DetailsBox->AddSlot().AutoHeight().Padding(0.0f, 3.0f)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(0.42f).VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
            [SNew(STextBlock).Text(FText::FromString(TEXT("Message")))]
            + SHorizontalBox::Slot().FillWidth(0.58f)
            [
                SNew(SEditableTextBox)
                .Text(FText::FromString(Node->Message))
                .OnTextCommitted_Lambda([this, NodeId = Node->NodeId](const FText& Text, ETextCommit::Type)
                {
                    CommitLogicMessage(NodeId, Text);
                })
            ]
        ];
        break;

    case EAkUGCLogicNodeType::Timer:
        DetailsBox->AddSlot().AutoHeight().Padding(0.0f, 3.0f)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(0.42f).VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
            [SNew(STextBlock).Text(FText::FromString(TEXT("Delay (seconds)")))]
            + SHorizontalBox::Slot().FillWidth(0.58f)
            [
                SNew(SNumericEntryBox<double>)
                .Value(Node->DelaySeconds)
                .MinValue(TOptional<double>(0.0))
                .OnValueCommitted_Lambda([this, NodeId = Node->NodeId](double Value, ETextCommit::Type)
                {
                    CommitLogicDelay(NodeId, Value);
                })
            ]
        ];
        break;

    case EAkUGCLogicNodeType::Spawn:
        DetailsBox->AddSlot().AutoHeight().Padding(0.0f, 3.0f)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(0.42f).VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
            [SNew(STextBlock).Text(FText::FromString(TEXT("Spawn Prefab")))]
            + SHorizontalBox::Slot().FillWidth(0.58f)
            [
                SNew(SComboButton)
                .OnGetMenuContent_Lambda([this, NodeId = Node->NodeId]()
                {
                    return BuildSpawnPrefabMenu(NodeId);
                })
                .ButtonContent()
                [
                    SNew(STextBlock)
                    .Text_Lambda([this, PrefabId = Node->SpawnPrefabId]()
                    {
                        return PrefabId.IsNone() ? FText::FromString(TEXT("(None)")) : FText::FromName(PrefabId);
                    })
                ]
            ]
        ];
        DetailsBox->AddSlot().AutoHeight().Padding(0.0f, 3.0f)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(0.42f).VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
            [SNew(STextBlock).Text(FText::FromString(TEXT("Anchor Entity")))]
            + SHorizontalBox::Slot().FillWidth(0.58f)
            [
                SNew(SComboButton)
                .OnGetMenuContent_Lambda([this, NodeId = Node->NodeId]()
                {
                    return BuildSpawnAnchorMenu(NodeId);
                })
                .ButtonContent()
                [
                    SNew(STextBlock)
                    .Text_Lambda([this, AnchorId = Node->SpawnAtEntityId]()
                    {
                        return FText::FromString(GetEntityDisplayName(AnchorId));
                    })
                ]
            ]
        ];
        break;
    }
}

FReply SAkUGCCreatorPanel::AddLogicNodeOfType(EAkUGCLogicNodeType Type)
{
    if (!Subsystem.IsValid())
    {
        Status = TEXT("Create or load a UGC project before editing logic.");
        return FReply::Handled();
    }

    FAkUGCLogicNode NewNode;
    NewNode.NodeId = FGuid::NewGuid();
    NewNode.Type = Type;
    if (Type == EAkUGCLogicNodeType::Spawn)
    {
        NewNode.SpawnPrefabId = TEXT("official.unit.basic_enemy");
    }

    const FAkUGCCommandExecutionResult Result = Subsystem->AddLogicNode(NewNode);
    Status = Result.bSucceeded
        ? FString::Printf(TEXT("Added %s logic node."), *GetLogicNodeTypeDisplayName(Type).ToString())
        : Result.ErrorMessage;
    if (Result.bSucceeded)
    {
        SelectedLogicNodeId = NewNode.NodeId;
        RefreshFromSubsystem();
    }
    return FReply::Handled();
}

FReply SAkUGCCreatorPanel::DeleteSelectedLogicNode()
{
    if (!SelectedLogicNodeId.IsValid())
    {
        Status = TEXT("Select a logic node to delete.");
        return FReply::Handled();
    }
    if (!Subsystem.IsValid())
    {
        return FReply::Handled();
    }

    const FAkUGCCommandExecutionResult Result = Subsystem->DeleteLogicNode(SelectedLogicNodeId);
    Status = Result.bSucceeded ? TEXT("Deleted logic node.") : Result.ErrorMessage;
    if (Result.bSucceeded)
    {
        SelectedLogicNodeId.Invalidate();
        RefreshFromSubsystem();
    }
    return FReply::Handled();
}

FReply SAkUGCCreatorPanel::ConnectPendingNodes()
{
    if (!PendingSourceNodeId.IsValid() || !PendingTargetNodeId.IsValid())
    {
        Status = TEXT("Choose a source and a target node to connect.");
        return FReply::Handled();
    }
    AddLogicConnection(PendingSourceNodeId, PendingTargetNodeId);
    return FReply::Handled();
}

void SAkUGCCreatorPanel::AddLogicConnection(const FGuid& SourceNodeId, const FGuid& TargetNodeId)
{
    if (!Subsystem.IsValid())
    {
        return;
    }
    const FAkUGCCommandExecutionResult Result = Subsystem->ConnectLogicNode(SourceNodeId, TargetNodeId);
    Status = Result.bSucceeded ? TEXT("Connected logic nodes.") : Result.ErrorMessage;
    if (Result.bSucceeded)
    {
        RefreshFromSubsystem();
    }
}

void SAkUGCCreatorPanel::RemoveLogicConnection(const FGuid& SourceNodeId, const FGuid& TargetNodeId)
{
    if (!Subsystem.IsValid())
    {
        return;
    }
    const FAkUGCCommandExecutionResult Result = Subsystem->DisconnectLogicNode(SourceNodeId, TargetNodeId);
    Status = Result.bSucceeded ? TEXT("Removed logic connection.") : Result.ErrorMessage;
    if (Result.bSucceeded)
    {
        RefreshFromSubsystem();
    }
}

void SAkUGCCreatorPanel::CommitLogicMessage(const FGuid& NodeId, const FText& Text)
{
    const FAkUGCLogicNode* Node = FindLogicNode(NodeId);
    if (!Node || !Subsystem.IsValid())
    {
        return;
    }
    FAkUGCLogicNode Updated = *Node;
    Updated.Message = Text.ToString();
    const FAkUGCCommandExecutionResult Result = Subsystem->UpdateLogicNode(Updated);
    Status = Result.bSucceeded ? TEXT("Updated logic node message.") : Result.ErrorMessage;
    if (Result.bSucceeded)
    {
        RefreshFromSubsystem();
    }
}

void SAkUGCCreatorPanel::CommitLogicDelay(const FGuid& NodeId, double Value)
{
    const FAkUGCLogicNode* Node = FindLogicNode(NodeId);
    if (!Node || !Subsystem.IsValid())
    {
        return;
    }
    FAkUGCLogicNode Updated = *Node;
    Updated.DelaySeconds = Value;
    const FAkUGCCommandExecutionResult Result = Subsystem->UpdateLogicNode(Updated);
    Status = Result.bSucceeded ? TEXT("Updated logic node delay.") : Result.ErrorMessage;
    if (Result.bSucceeded)
    {
        RefreshFromSubsystem();
    }
}

void SAkUGCCreatorPanel::CommitLogicSpawnPrefab(const FGuid& NodeId, FName PrefabId)
{
    const FAkUGCLogicNode* Node = FindLogicNode(NodeId);
    if (!Node || !Subsystem.IsValid())
    {
        return;
    }
    FAkUGCLogicNode Updated = *Node;
    Updated.SpawnPrefabId = PrefabId;
    const FAkUGCCommandExecutionResult Result = Subsystem->UpdateLogicNode(Updated);
    Status = Result.bSucceeded ? TEXT("Updated spawn prefab.") : Result.ErrorMessage;
    if (Result.bSucceeded)
    {
        RefreshFromSubsystem();
    }
}

void SAkUGCCreatorPanel::CommitLogicSpawnAnchor(const FGuid& NodeId, const FGuid& AnchorEntityId)
{
    const FAkUGCLogicNode* Node = FindLogicNode(NodeId);
    if (!Node || !Subsystem.IsValid())
    {
        return;
    }
    FAkUGCLogicNode Updated = *Node;
    Updated.SpawnAtEntityId = AnchorEntityId;
    const FAkUGCCommandExecutionResult Result = Subsystem->UpdateLogicNode(Updated);
    Status = Result.bSucceeded ? TEXT("Updated spawn anchor entity.") : Result.ErrorMessage;
    if (Result.bSucceeded)
    {
        RefreshFromSubsystem();
    }
}

TSharedRef<SWidget> SAkUGCCreatorPanel::BuildAddLogicNodeMenu()
{
    FMenuBuilder MenuBuilder(true, nullptr);
    const TWeakPtr<SAkUGCCreatorPanel> WeakThis = SharedThis(this);
    const TArray<EAkUGCLogicNodeType> Types = {
        EAkUGCLogicNodeType::GameStart,
        EAkUGCLogicNodeType::WaveStart,
        EAkUGCLogicNodeType::Message,
        EAkUGCLogicNodeType::Timer,
        EAkUGCLogicNodeType::Spawn
    };
    for (const EAkUGCLogicNodeType Type : Types)
    {
        MenuBuilder.AddMenuEntry(
            GetLogicNodeTypeDisplayName(Type),
            FText::GetEmpty(),
            FSlateIcon(),
            FUIAction(FExecuteAction::CreateLambda([WeakThis, Type]()
            {
                if (const TSharedPtr<SAkUGCCreatorPanel> Panel = WeakThis.Pin())
                {
                    Panel->AddLogicNodeOfType(Type);
                }
            })));
    }
    return MenuBuilder.MakeWidget();
}

TSharedRef<SWidget> SAkUGCCreatorPanel::BuildSpawnPrefabMenu(const FGuid& NodeId)
{
    FMenuBuilder MenuBuilder(true, nullptr);
    const TWeakPtr<SAkUGCCreatorPanel> WeakThis = SharedThis(this);
    for (const FName PrefabId : FAkUGCOfficialPrefabCatalog::GetTowerDefensePrefabIds())
    {
        MenuBuilder.AddMenuEntry(
            FText::FromName(PrefabId),
            FText::GetEmpty(),
            FSlateIcon(),
            FUIAction(FExecuteAction::CreateLambda([WeakThis, NodeId, PrefabId]()
            {
                if (const TSharedPtr<SAkUGCCreatorPanel> Panel = WeakThis.Pin())
                {
                    Panel->CommitLogicSpawnPrefab(NodeId, PrefabId);
                }
            })));
    }
    return MenuBuilder.MakeWidget();
}

TSharedRef<SWidget> SAkUGCCreatorPanel::BuildSpawnAnchorMenu(const FGuid& NodeId)
{
    FMenuBuilder MenuBuilder(true, nullptr);
    const TWeakPtr<SAkUGCCreatorPanel> WeakThis = SharedThis(this);
    MenuBuilder.AddMenuEntry(
        FText::FromString(TEXT("(None)")),
        FText::FromString(TEXT("Spawn at the world origin when no anchor entity is selected.")),
        FSlateIcon(),
        FUIAction(FExecuteAction::CreateLambda([WeakThis, NodeId]()
        {
            if (const TSharedPtr<SAkUGCCreatorPanel> Panel = WeakThis.Pin())
            {
                Panel->CommitLogicSpawnAnchor(NodeId, FGuid{});
            }
        })));
    if (Subsystem.IsValid() && !Subsystem->GetDocument().Scenes.IsEmpty())
    {
        for (const FAkUGCEntityRecord& Entity : Subsystem->GetDocument().Scenes[0].Entities)
        {
            const FGuid EntityId = Entity.EntityId;
            const FString DisplayName = GetEntityDisplayName(EntityId);
            MenuBuilder.AddMenuEntry(
                FText::FromString(DisplayName),
                FText::GetEmpty(),
                FSlateIcon(),
                FUIAction(FExecuteAction::CreateLambda([WeakThis, NodeId, EntityId]()
                {
                    if (const TSharedPtr<SAkUGCCreatorPanel> Panel = WeakThis.Pin())
                    {
                        Panel->CommitLogicSpawnAnchor(NodeId, EntityId);
                    }
                })));
        }
    }
    return MenuBuilder.MakeWidget();
}

TSharedRef<SWidget> SAkUGCCreatorPanel::BuildSourceNodeMenu()
{
    FMenuBuilder MenuBuilder(true, nullptr);
    const TWeakPtr<SAkUGCCreatorPanel> WeakThis = SharedThis(this);
    const FAkUGCLogicGraph* Graph = Subsystem.IsValid() ? Subsystem->GetLogicGraph() : nullptr;
    if (Graph)
    {
        for (const FAkUGCLogicNode& Node : Graph->Nodes)
        {
            const FGuid NodeId = Node.NodeId;
            const FText Label = GetLogicNodeLabel(Node);
            MenuBuilder.AddMenuEntry(
                Label,
                FText::GetEmpty(),
                FSlateIcon(),
                FUIAction(FExecuteAction::CreateLambda([WeakThis, NodeId]()
                {
                    if (const TSharedPtr<SAkUGCCreatorPanel> Panel = WeakThis.Pin())
                    {
                        Panel->PendingSourceNodeId = NodeId;
                    }
                })));
        }
    }
    return MenuBuilder.MakeWidget();
}

TSharedRef<SWidget> SAkUGCCreatorPanel::BuildTargetNodeMenu()
{
    FMenuBuilder MenuBuilder(true, nullptr);
    const TWeakPtr<SAkUGCCreatorPanel> WeakThis = SharedThis(this);
    const FAkUGCLogicGraph* Graph = Subsystem.IsValid() ? Subsystem->GetLogicGraph() : nullptr;
    if (Graph)
    {
        for (const FAkUGCLogicNode& Node : Graph->Nodes)
        {
            const FGuid NodeId = Node.NodeId;
            const FText Label = GetLogicNodeLabel(Node);
            MenuBuilder.AddMenuEntry(
                Label,
                FText::GetEmpty(),
                FSlateIcon(),
                FUIAction(FExecuteAction::CreateLambda([WeakThis, NodeId]()
                {
                    if (const TSharedPtr<SAkUGCCreatorPanel> Panel = WeakThis.Pin())
                    {
                        Panel->PendingTargetNodeId = NodeId;
                    }
                })));
        }
    }
    return MenuBuilder.MakeWidget();
}

FReply SAkUGCCreatorPanel::ToggleLogicGraphView()
{
    bShowingLogicGraph = !bShowingLogicGraph;
    if (LogicViewSwitcher.IsValid())
    {
        LogicViewSwitcher->SetActiveWidgetIndex(bShowingLogicGraph ? 1 : 0);
    }
    return FReply::Handled();
}

FText SAkUGCCreatorPanel::GetLogicViewToggleLabel() const
{
    return FText::FromString(bShowingLogicGraph ? TEXT("List") : TEXT("Graph"));
}

FText SAkUGCCreatorPanel::GetPendingSourceLabel() const
{
    const FAkUGCLogicNode* Node = FindLogicNode(PendingSourceNodeId);
    return Node ? GetLogicNodeLabel(*Node) : FText::FromString(TEXT("Source"));
}

FText SAkUGCCreatorPanel::GetPendingTargetLabel() const
{
    const FAkUGCLogicNode* Node = FindLogicNode(PendingTargetNodeId);
    return Node ? GetLogicNodeLabel(*Node) : FText::FromString(TEXT("Target"));
}

FText SAkUGCCreatorPanel::GetLogicValidationText() const
{
    if (!Subsystem.IsValid())
    {
        return FText::FromString(TEXT("Create or load a UGC project to validate its logic graph."));
    }

    const FAkUGCValidationResult Result = Subsystem->ValidateLogicGraph();
    if (Result.IsValid())
    {
        return FText::FromString(TEXT("Logic graph: valid."));
    }

    FString Text = FString::Printf(TEXT("Logic graph has %d issue(s):"), Result.Issues.Num());
    for (const FAkUGCValidationIssue& Issue : Result.Issues)
    {
        Text += FString::Printf(
            TEXT("\n- [%s] %s"),
            Issue.Severity == EAkUGCValidationSeverity::Error ? TEXT("Error") : TEXT("Warning"),
            *Issue.Message);
    }
    return FText::FromString(Text);
}

const FAkUGCLogicNode* SAkUGCCreatorPanel::FindLogicNode(const FGuid& NodeId) const
{
    const FAkUGCLogicGraph* Graph = Subsystem.IsValid() ? Subsystem->GetLogicGraph() : nullptr;
    return Graph ? Graph->Nodes.FindByPredicate([&NodeId](const FAkUGCLogicNode& Node)
    {
        return Node.NodeId == NodeId;
    }) : nullptr;
}

FString SAkUGCCreatorPanel::GetEntityDisplayName(const FGuid& EntityId) const
{
    if (!EntityId.IsValid())
    {
        return TEXT("(None)");
    }
    const FAkUGCEntityRecord* Entity = Subsystem.IsValid() ? Subsystem->FindEntity(EntityId) : nullptr;
    if (!Entity)
    {
        return TEXT("(Missing Entity)");
    }
    const FAkUGCPrefabDefinition* Prefab = Subsystem->FindPrefabForEntity(EntityId);
    const FString Name = Prefab ? Prefab->DisplayName : Entity->PrefabId.ToString();
    return FString::Printf(TEXT("%s  [%s]"), *Name, *EntityId.ToString(EGuidFormats::Short));
}

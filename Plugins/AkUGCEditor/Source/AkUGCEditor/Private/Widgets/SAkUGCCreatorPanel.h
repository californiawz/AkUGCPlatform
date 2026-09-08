#pragma once

#include "CoreMinimal.h"
#include "Document/AkUGCDocument.h"
#include "Prefab/AkUGCPrefabDefinition.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STreeView.h"

class SVerticalBox;
class SWidgetSwitcher;
class SAkUGCLogicCanvas;
class UAkUGCEditorSubsystem;

struct FAkUGCEntityTreeItem
{
    FGuid EntityId;
    TArray<TSharedPtr<FAkUGCEntityTreeItem>> Children;
};

class SAkUGCCreatorPanel final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SAkUGCCreatorPanel) {}
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs, UAkUGCEditorSubsystem* InSubsystem);
    virtual void Tick(const FGeometry& AllottedGeometry, double InCurrentTime, float InDeltaTime) override;

private:
    FReply NewProject();
    FReply SaveProject();
    FReply LoadProject();
    FReply Undo();
    FReply Redo();
    FReply DeleteSelected();
    FReply DuplicateSelected();
    FReply PlacePrefab(FName PrefabId);
    FText GetStatusText() const;

    void RefreshFromSubsystem();
    void RebuildEntityTree();
    void RebuildDetails();
    void SynchronizeTreeSelection();
    TSharedRef<ITableRow> GenerateEntityRow(
        TSharedPtr<FAkUGCEntityTreeItem> Item,
        const TSharedRef<STableViewBase>& OwnerTable) const;
    void GetEntityChildren(
        TSharedPtr<FAkUGCEntityTreeItem> Item,
        TArray<TSharedPtr<FAkUGCEntityTreeItem>>& OutChildren) const;
    void OnEntitySelectionChanged(TSharedPtr<FAkUGCEntityTreeItem> Item, ESelectInfo::Type SelectInfo);
    FText GetEntityLabel(TSharedPtr<FAkUGCEntityTreeItem> Item) const;
    FText GetParentLabel(const FGuid& EntityId) const;
    TSharedRef<SWidget> BuildParentMenu(const FGuid& EntityId);
    void CommitParent(const FGuid& EntityId, const FGuid& ParentEntityId);
    bool IsValidParentCandidate(const FGuid& EntityId, const FGuid& CandidateParentId) const;
    TSharedRef<SWidget> BuildPropertyEditor(
        const FGuid& EntityId,
        const FAkUGCPropertyDefinition& Property,
        const FAkUGCValue& Value);
    void CommitProperty(
        const FGuid& EntityId,
        const FAkUGCPropertyDefinition& Property,
        const FAkUGCValue& Value);
    static const FAkUGCValue* FindPropertyValue(
        const FAkUGCEntityRecord& Entity,
        const FAkUGCPropertyDefinition& Property);

    // Logic editing
    void RebuildLogicList();
    void RebuildLogicDetails();
    void RebuildConnections();
    TSharedRef<ITableRow> GenerateLogicRow(
        TSharedPtr<FAkUGCLogicNode> Item,
        const TSharedRef<STableViewBase>& OwnerTable) const;
    void OnLogicSelectionChanged(TSharedPtr<FAkUGCLogicNode> Item, ESelectInfo::Type SelectInfo);
    FText GetLogicNodeLabel(const FAkUGCLogicNode& Node) const;
    FReply AddLogicNodeOfType(EAkUGCLogicNodeType Type);
    FReply DeleteSelectedLogicNode();
    FReply ConnectPendingNodes();
    void CommitLogicMessage(const FGuid& NodeId, const FText& Text);
    void CommitLogicDelay(const FGuid& NodeId, double Value);
    void CommitLogicSpawnPrefab(const FGuid& NodeId, FName PrefabId);
    void CommitLogicSpawnAnchor(const FGuid& NodeId, const FGuid& AnchorEntityId);
    void AddLogicConnection(const FGuid& SourceNodeId, const FGuid& TargetNodeId);
    void RemoveLogicConnection(const FGuid& SourceNodeId, const FGuid& TargetNodeId);
    TSharedRef<SWidget> BuildAddLogicNodeMenu();
    TSharedRef<SWidget> BuildSpawnPrefabMenu(const FGuid& NodeId);
    TSharedRef<SWidget> BuildSpawnAnchorMenu(const FGuid& NodeId);
    TSharedRef<SWidget> BuildSourceNodeMenu();
    TSharedRef<SWidget> BuildTargetNodeMenu();
    FReply ToggleLogicGraphView();
    FText GetLogicViewToggleLabel() const;
    FText GetPendingSourceLabel() const;
    FText GetPendingTargetLabel() const;
    FText GetLogicValidationText() const;
    static FText GetLogicNodeTypeDisplayName(EAkUGCLogicNodeType Type);
    const FAkUGCLogicNode* FindLogicNode(const FGuid& NodeId) const;
    FString GetEntityDisplayName(const FGuid& EntityId) const;

    TWeakObjectPtr<UAkUGCEditorSubsystem> Subsystem;
    TSharedPtr<STreeView<TSharedPtr<FAkUGCEntityTreeItem>>> EntityTreeView;
    TSharedPtr<SVerticalBox> DetailsBox;
    TArray<TSharedPtr<FAkUGCEntityTreeItem>> EntityRoots;
    TMap<FGuid, TSharedPtr<FAkUGCEntityTreeItem>> EntityItemsById;
    FString Status;
    uint64 ObservedDocumentRevision = MAX_uint64;
    FGuid ObservedSelectionId;
    int32 PlacementIndex = 0;
    bool bUpdatingTreeSelection = false;

    TSharedPtr<SListView<TSharedPtr<FAkUGCLogicNode>>> LogicListView;
    TArray<TSharedPtr<FAkUGCLogicNode>> LogicItems;
    TSharedPtr<SVerticalBox> ConnectionsBox;
    TSharedPtr<SWidgetSwitcher> LogicViewSwitcher;
    TSharedPtr<SAkUGCLogicCanvas> LogicCanvas;
    FGuid SelectedLogicNodeId;
    FGuid PendingSourceNodeId;
    FGuid PendingTargetNodeId;
    bool bUpdatingLogicSelection = false;
    bool bShowingLogicGraph = false;
};

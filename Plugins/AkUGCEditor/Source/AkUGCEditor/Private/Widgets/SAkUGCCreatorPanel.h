#pragma once

#include "CoreMinimal.h"
#include "Document/AkUGCDocument.h"
#include "Prefab/AkUGCPrefabDefinition.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/STreeView.h"

class SVerticalBox;
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
};

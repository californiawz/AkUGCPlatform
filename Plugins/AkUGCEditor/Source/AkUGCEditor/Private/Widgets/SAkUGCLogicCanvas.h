#pragma once

#include "CoreMinimal.h"
#include "Document/AkUGCDocument.h"
#include "Widgets/SCompoundWidget.h"

class UAkUGCEditorSubsystem;

/**
 * Creator Studio 的 Trigger Graph 可视化画布。
 *
 * 以可视化节点与连线呈现 Scene 的 Logic Graph，并支持：
 *  - 拖动节点调整布局（松手时提交 SetLogicNodePosition 命令，布局可持久化）
 *  - 从输出 Pin 拖拽连线到输入 Pin（松手时提交 ConnectLogicNode 命令）
 *
 * 该画布不保存任何自有数据副本，节点位置与连接均读取自 UGC Project Document
 * 单一真源，通过 EditorSubsystem 的命令接口写入，天然支持 Undo/Redo。
 */
class SAkUGCLogicCanvas final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SAkUGCLogicCanvas) {}
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs, UAkUGCEditorSubsystem* InSubsystem);

    virtual int32 OnPaint(
        const FPaintArgs& Args,
        const FGeometry& AllottedGeometry,
        const FSlateRect& MyCullingRect,
        FSlateWindowElementList& OutDrawElements,
        int32 LayerId,
        const FWidgetStyle& InWidgetStyle,
        bool bParentEnabled) const override;
    virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
    virtual FReply OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
    virtual FReply OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
    virtual FVector2D ComputeDesiredSize(float LayoutScaleMultiplier) const override;
    virtual void Tick(const FGeometry& AllottedGeometry, double InCurrentTime, float InDeltaTime) override;

private:
    static constexpr float NodeWidth = 180.0f;
    static constexpr float NodeHeight = 64.0f;
    static constexpr float PinRadius = 7.0f;

    struct FNodeRenderInfo
    {
        FGuid NodeId;
        FVector2D Position;
        FLinearColor Color;
        FString Title;
        FString Subtitle;
        bool bHasInputPin;
    };

    void GatherRenderInfos(TArray<FNodeRenderInfo>& OutInfos) const;
    FVector2D GetOutputPinPosition(const FNodeRenderInfo& Info) const;
    FVector2D GetInputPinPosition(const FNodeRenderInfo& Info) const;
    static FLinearColor GetNodeColor(EAkUGCLogicNodeType Type);
    static FString GetNodeTitle(EAkUGCLogicNodeType Type);
    static FString GetNodeSubtitle(const FAkUGCLogicNode& Node);
    bool HitTestNode(const FNodeRenderInfo& Info, const FVector2D& Point) const;
    bool HitTestPin(const FVector2D& PinCenter, const FVector2D& Point) const;
    void DrawConnection(
        const FVector2D& Start,
        const FVector2D& End,
        const FLinearColor& Color,
        const FGeometry& AllottedGeometry,
        FSlateWindowElementList& OutDrawElements,
        int32 LayerId) const;

    TWeakObjectPtr<UAkUGCEditorSubsystem> Subsystem;
    uint64 ObservedDocumentRevision = MAX_uint64;

    FGuid DraggingNodeId;
    FVector2D DragGrabOffset;
    TOptional<FVector2D> DragPreviewPosition;
    FGuid ConnectingSourceNodeId;
    FVector2D CursorPosition;
};

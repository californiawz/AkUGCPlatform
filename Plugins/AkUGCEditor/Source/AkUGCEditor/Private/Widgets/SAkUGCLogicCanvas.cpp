#include "Widgets/SAkUGCLogicCanvas.h"

#include "Fonts/SlateFontInfo.h"
#include "Rendering/DrawElements.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Subsystem/AkUGCEditorSubsystem.h"

void SAkUGCLogicCanvas::Construct(const FArguments& InArgs, UAkUGCEditorSubsystem* InSubsystem)
{
    Subsystem = InSubsystem;
    if (Subsystem.IsValid())
    {
        ObservedDocumentRevision = Subsystem->GetDocumentRevision();
    }
}

int32 SAkUGCLogicCanvas::OnPaint(
    const FPaintArgs& Args,
    const FGeometry& AllottedGeometry,
    const FSlateRect& MyCullingRect,
    FSlateWindowElementList& OutDrawElements,
    int32 LayerId,
    const FWidgetStyle& InWidgetStyle,
    bool bParentEnabled) const
{
    const int32 BackgroundLayer = LayerId;
    const int32 ConnectionLayer = LayerId + 1;
    const int32 NodeLayer = LayerId + 2;
    const int32 NodeTitleLayer = LayerId + 3;
    const int32 PinLayer = LayerId + 4;
    const int32 TextLayer = LayerId + 5;

    const FVector2D LocalSize = AllottedGeometry.GetLocalSize();

    // 画布背景，确保空白区域也能命中鼠标。
    FSlateDrawElement::MakeBox(
        OutDrawElements,
        BackgroundLayer,
        AllottedGeometry.ToPaintGeometry(),
        FAppStyle::GetBrush(TEXT("WhiteBrush")),
        ESlateDrawEffect::None,
        FLinearColor(0.03f, 0.03f, 0.035f));

    TArray<FNodeRenderInfo> Infos;
    GatherRenderInfos(Infos);

    // 1) 连接线（在节点下方）。
    const FAkUGCLogicGraph* Graph = Subsystem.IsValid() ? Subsystem->GetLogicGraph() : nullptr;
    if (Graph)
    {
        for (const FAkUGCLogicConnection& Connection : Graph->Connections)
        {
            const FNodeRenderInfo* Source = Infos.FindByPredicate(
                [&Connection](const FNodeRenderInfo& Info) { return Info.NodeId == Connection.SourceNodeId; });
            const FNodeRenderInfo* Target = Infos.FindByPredicate(
                [&Connection](const FNodeRenderInfo& Info) { return Info.NodeId == Connection.TargetNodeId; });
            if (Source && Target && Target->bHasInputPin)
            {
                DrawConnection(
                    GetOutputPinPosition(*Source),
                    GetInputPinPosition(*Target),
                    FLinearColor(0.7f, 0.7f, 0.75f),
                    AllottedGeometry,
                    OutDrawElements,
                    ConnectionLayer);
            }
        }
    }

    // 2) 拖拽中的临时连线。
    if (ConnectingSourceNodeId.IsValid())
    {
        const FNodeRenderInfo* Source = Infos.FindByPredicate(
            [this](const FNodeRenderInfo& Info) { return Info.NodeId == ConnectingSourceNodeId; });
        if (Source)
        {
            DrawConnection(
                GetOutputPinPosition(*Source),
                CursorPosition,
                FLinearColor(1.0f, 0.85f, 0.2f),
                AllottedGeometry,
                OutDrawElements,
                ConnectionLayer);
        }
    }

    // 3) 节点本体与 Pin。
    const FSlateFontInfo TitleFont = FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 11);
    const FSlateFontInfo SubtitleFont = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 9);

    for (const FNodeRenderInfo& Info : Infos)
    {
        const FVector2D NodeSize(NodeWidth, NodeHeight);

        FSlateDrawElement::MakeBox(
            OutDrawElements,
            NodeLayer,
            AllottedGeometry.ToPaintGeometry(NodeSize, FSlateLayoutTransform(Info.Position)),
            FAppStyle::GetBrush(TEXT("WhiteBrush")),
            ESlateDrawEffect::None,
            Info.Color);

        const FVector2D TitleSize(NodeWidth, 22.0f);
        FSlateDrawElement::MakeBox(
            OutDrawElements,
            NodeTitleLayer,
            AllottedGeometry.ToPaintGeometry(TitleSize, FSlateLayoutTransform(Info.Position)),
            FAppStyle::GetBrush(TEXT("WhiteBrush")),
            ESlateDrawEffect::None,
            FLinearColor::LerpUsingHSV(Info.Color, FLinearColor::White, 0.15f));

        FSlateDrawElement::MakeText(
            OutDrawElements,
            TextLayer,
            AllottedGeometry.ToPaintGeometry(FVector2D(1.0f, 1.0f), FSlateLayoutTransform(Info.Position + FVector2D(8.0f, 3.0f))),
            Info.Title,
            TitleFont,
            ESlateDrawEffect::None,
            FLinearColor::White);

        if (!Info.Subtitle.IsEmpty())
        {
            FSlateDrawElement::MakeText(
                OutDrawElements,
                TextLayer,
                AllottedGeometry.ToPaintGeometry(FVector2D(1.0f, 1.0f), FSlateLayoutTransform(Info.Position + FVector2D(8.0f, 30.0f))),
                Info.Subtitle,
                SubtitleFont,
                ESlateDrawEffect::None,
                FLinearColor(0.9f, 0.9f, 0.9f));
        }

        // 输出 Pin（所有节点都有）。
        const FVector2D OutputPin = GetOutputPinPosition(Info);
        FSlateDrawElement::MakeBox(
            OutDrawElements,
            PinLayer,
            AllottedGeometry.ToPaintGeometry(
                FVector2D(PinRadius * 2.0f, PinRadius * 2.0f),
                FSlateLayoutTransform(OutputPin - FVector2D(PinRadius, PinRadius))),
            FAppStyle::GetBrush(TEXT("WhiteBrush")),
            ESlateDrawEffect::None,
            FLinearColor::White);

        // 输入 Pin（入口节点没有）。
        if (Info.bHasInputPin)
        {
            const FVector2D InputPin = GetInputPinPosition(Info);
            FSlateDrawElement::MakeBox(
                OutDrawElements,
                PinLayer,
                AllottedGeometry.ToPaintGeometry(
                    FVector2D(PinRadius * 2.0f, PinRadius * 2.0f),
                    FSlateLayoutTransform(InputPin - FVector2D(PinRadius, PinRadius))),
                FAppStyle::GetBrush(TEXT("WhiteBrush")),
                ESlateDrawEffect::None,
                FLinearColor::White);
        }
    }

    return TextLayer + 1;
}

FVector2D SAkUGCLogicCanvas::ComputeDesiredSize(float LayoutScaleMultiplier) const
{
    return FVector2D(720.0f, 480.0f);
}

void SAkUGCLogicCanvas::Tick(
    const FGeometry& AllottedGeometry,
    double InCurrentTime,
    float InDeltaTime)
{
    SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);

    if (Subsystem.IsValid() && ObservedDocumentRevision != Subsystem->GetDocumentRevision())
    {
        ObservedDocumentRevision = Subsystem->GetDocumentRevision();
        Invalidate(EInvalidateWidgetReason::Paint);
    }
}

FReply SAkUGCLogicCanvas::OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
    if (MouseEvent.GetEffectingButton() != EKeys::LeftMouseButton || !Subsystem.IsValid())
    {
        return FReply::Unhandled();
    }

    const FVector2D MousePos = MyGeometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition());
    TArray<FNodeRenderInfo> Infos;
    GatherRenderInfos(Infos);

    // 优先命中输出 Pin，开始拖拽连线。
    for (const FNodeRenderInfo& Info : Infos)
    {
        if (HitTestPin(GetOutputPinPosition(Info), MousePos))
        {
            ConnectingSourceNodeId = Info.NodeId;
            CursorPosition = MousePos;
            return FReply::Handled().CaptureMouse(SharedThis(this));
        }
    }

    // 再命中节点本体（逆序，视觉上层优先）。
    for (int32 Index = Infos.Num() - 1; Index >= 0; --Index)
    {
        const FNodeRenderInfo& Info = Infos[Index];
        if (HitTestNode(Info, MousePos))
        {
            DraggingNodeId = Info.NodeId;
            DragGrabOffset = MousePos - Info.Position;
            DragPreviewPosition = Info.Position;
            return FReply::Handled().CaptureMouse(SharedThis(this));
        }
    }

    return FReply::Unhandled();
}

FReply SAkUGCLogicCanvas::OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
    const FVector2D MousePos = MyGeometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition());

    if (DraggingNodeId.IsValid())
    {
        DragPreviewPosition = MousePos - DragGrabOffset;
        return FReply::Handled();
    }
    if (ConnectingSourceNodeId.IsValid())
    {
        CursorPosition = MousePos;
        return FReply::Handled();
    }

    return FReply::Unhandled();
}

FReply SAkUGCLogicCanvas::OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
    if (MouseEvent.GetEffectingButton() != EKeys::LeftMouseButton || !Subsystem.IsValid())
    {
        return FReply::Unhandled();
    }

    const FVector2D MousePos = MyGeometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition());

    if (DraggingNodeId.IsValid())
    {
        const FVector2D FinalPosition = MousePos - DragGrabOffset;
        Subsystem->SetLogicNodePosition(DraggingNodeId, FinalPosition.X, FinalPosition.Y);
        DraggingNodeId.Invalidate();
        DragPreviewPosition.Reset();
        return FReply::Handled().ReleaseMouseCapture();
    }

    if (ConnectingSourceNodeId.IsValid())
    {
        TArray<FNodeRenderInfo> Infos;
        GatherRenderInfos(Infos);
        for (const FNodeRenderInfo& Info : Infos)
        {
            if (Info.bHasInputPin
                && Info.NodeId != ConnectingSourceNodeId
                && HitTestPin(GetInputPinPosition(Info), MousePos))
            {
                Subsystem->ConnectLogicNode(ConnectingSourceNodeId, Info.NodeId);
                break;
            }
        }
        ConnectingSourceNodeId.Invalidate();
        CursorPosition = FVector2D::ZeroVector;
        return FReply::Handled().ReleaseMouseCapture();
    }

    return FReply::Unhandled();
}

void SAkUGCLogicCanvas::GatherRenderInfos(TArray<FNodeRenderInfo>& OutInfos) const
{
    OutInfos.Reset();

    const FAkUGCLogicGraph* Graph = Subsystem.IsValid() ? Subsystem->GetLogicGraph() : nullptr;
    if (!Graph)
    {
        return;
    }

    for (const FAkUGCLogicNode& Node : Graph->Nodes)
    {
        FNodeRenderInfo Info;
        Info.NodeId = Node.NodeId;
        Info.Position = FVector2D(Node.PositionX, Node.PositionY);
        Info.Color = GetNodeColor(Node.Type);
        Info.Title = GetNodeTitle(Node.Type);
        Info.Subtitle = GetNodeSubtitle(Node);
        Info.bHasInputPin = Node.Type != EAkUGCLogicNodeType::GameStart
            && Node.Type != EAkUGCLogicNodeType::WaveStart;

        if (Node.NodeId == DraggingNodeId && DragPreviewPosition.IsSet())
        {
            Info.Position = DragPreviewPosition.GetValue();
        }

        OutInfos.Add(MoveTemp(Info));
    }
}

FVector2D SAkUGCLogicCanvas::GetOutputPinPosition(const FNodeRenderInfo& Info) const
{
    return Info.Position + FVector2D(NodeWidth, NodeHeight * 0.5f);
}

FVector2D SAkUGCLogicCanvas::GetInputPinPosition(const FNodeRenderInfo& Info) const
{
    return Info.Position + FVector2D(0.0f, NodeHeight * 0.5f);
}

FLinearColor SAkUGCLogicCanvas::GetNodeColor(EAkUGCLogicNodeType Type)
{
    switch (Type)
    {
    case EAkUGCLogicNodeType::GameStart: return FLinearColor(0.15f, 0.55f, 0.20f);
    case EAkUGCLogicNodeType::WaveStart: return FLinearColor(0.15f, 0.40f, 0.80f);
    case EAkUGCLogicNodeType::Message: return FLinearColor(0.45f, 0.45f, 0.50f);
    case EAkUGCLogicNodeType::Timer: return FLinearColor(0.85f, 0.50f, 0.10f);
    case EAkUGCLogicNodeType::Spawn: return FLinearColor(0.55f, 0.25f, 0.75f);
    default: return FLinearColor(0.30f, 0.30f, 0.30f);
    }
}

FString SAkUGCLogicCanvas::GetNodeTitle(EAkUGCLogicNodeType Type)
{
    switch (Type)
    {
    case EAkUGCLogicNodeType::GameStart: return TEXT("Game Start");
    case EAkUGCLogicNodeType::WaveStart: return TEXT("Wave Start");
    case EAkUGCLogicNodeType::Message: return TEXT("Message");
    case EAkUGCLogicNodeType::Timer: return TEXT("Timer");
    case EAkUGCLogicNodeType::Spawn: return TEXT("Spawn");
    default: return TEXT("Unknown");
    }
}

FString SAkUGCLogicCanvas::GetNodeSubtitle(const FAkUGCLogicNode& Node)
{
    switch (Node.Type)
    {
    case EAkUGCLogicNodeType::Message: return Node.Message;
    case EAkUGCLogicNodeType::Timer: return FString::Printf(TEXT("%.1f s"), Node.DelaySeconds);
    case EAkUGCLogicNodeType::Spawn: return Node.SpawnPrefabId.ToString();
    default: return FString();
    }
}

bool SAkUGCLogicCanvas::HitTestNode(const FNodeRenderInfo& Info, const FVector2D& Point) const
{
    return Point.X >= Info.Position.X
        && Point.X <= Info.Position.X + NodeWidth
        && Point.Y >= Info.Position.Y
        && Point.Y <= Info.Position.Y + NodeHeight;
}

bool SAkUGCLogicCanvas::HitTestPin(const FVector2D& PinCenter, const FVector2D& Point) const
{
    return FVector2D::Distance(Point, PinCenter) <= PinRadius + 2.0f;
}

void SAkUGCLogicCanvas::DrawConnection(
    const FVector2D& Start,
    const FVector2D& End,
    const FLinearColor& Color,
    const FGeometry& AllottedGeometry,
    FSlateWindowElementList& OutDrawElements,
    int32 LayerId) const
{
    const float Tangent = FMath::Clamp(FMath::Abs(End.X - Start.X) * 0.5f, 40.0f, 200.0f);
    const FVector2D ControlA(Start.X + Tangent, Start.Y);
    const FVector2D ControlB(End.X - Tangent, End.Y);

    TArray<FVector2D> Points;
    constexpr int32 SegmentCount = 24;
    Points.Reserve(SegmentCount + 1);
    for (int32 Index = 0; Index <= SegmentCount; ++Index)
    {
        const float T = static_cast<float>(Index) / static_cast<float>(SegmentCount);
        const float InvT = 1.0f - T;
        const FVector2D Point =
            InvT * InvT * InvT * Start
            + 3.0f * InvT * InvT * T * ControlA
            + 3.0f * InvT * T * T * ControlB
            + T * T * T * End;
        Points.Add(Point);
    }

    FSlateDrawElement::MakeLines(
        OutDrawElements,
        LayerId,
        AllottedGeometry.ToPaintGeometry(),
        Points,
        ESlateDrawEffect::None,
        Color,
        true,
        2.0f);
}

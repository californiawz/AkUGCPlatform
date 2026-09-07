#include "Gameplay/AkUGCTowerDefensePath.h"

#include "Document/AkUGCDocument.h"

namespace
{
    constexpr int64 MinimumPathOrder = 0;
    constexpr int64 MaximumPathOrder = 10000;
    constexpr double MinimumPathSegmentLength = 1.0;

    FAkUGCTowerDefensePathBuildResult Failure(FString Path, FString Message, bool bPathPresent)
    {
        FAkUGCTowerDefensePathBuildResult Result;
        Result.bPathPresent = bPathPresent;
        Result.ErrorPath = MoveTemp(Path);
        Result.ErrorMessage = MoveTemp(Message);
        return Result;
    }

    FString EntityPath(int32 EntityIndex)
    {
        return FString::Printf(TEXT("entities[%d]"), EntityIndex);
    }

    bool GuidLess(const FGuid& Left, const FGuid& Right)
    {
        return Left.ToString(EGuidFormats::Digits) < Right.ToString(EGuidFormats::Digits);
    }
}

bool FAkUGCTowerDefensePath::IsEmpty() const
{
    return Nodes.IsEmpty();
}

int32 FAkUGCTowerDefensePath::Num() const
{
    return Nodes.Num();
}

const FAkUGCTowerDefensePathNode* FAkUGCTowerDefensePath::GetNode(int32 Index) const
{
    return Nodes.IsValidIndex(Index) ? &Nodes[Index] : nullptr;
}

FAkUGCTowerDefensePathBuildResult FAkUGCTowerDefensePathBuilder::Build(
    const FAkUGCSceneDocument& Scene,
    bool bRequireUsablePath)
{
    struct FCandidate
    {
        FAkUGCTowerDefensePathNode Node;
        int32 EntityIndex = INDEX_NONE;
        FString OrderPath;
    };

    TArray<FCandidate> Candidates;
    TSet<FGuid> PathEntityIds;
    for (int32 EntityIndex = 0; EntityIndex < Scene.Entities.Num(); ++EntityIndex)
    {
        const FAkUGCEntityRecord& Entity = Scene.Entities[EntityIndex];
        if (Entity.PrefabId != TEXT("official.gameplay.path_node"))
        {
            continue;
        }

        const FString Path = EntityPath(EntityIndex);
        if (!Entity.EntityId.IsValid())
        {
            return Failure(Path + TEXT(".entityId"), TEXT("Path node Entity ID must be a valid GUID."), true);
        }
        if (PathEntityIds.Contains(Entity.EntityId))
        {
            return Failure(Path + TEXT(".entityId"), TEXT("Path node Entity ID must be unique."), true);
        }
        PathEntityIds.Add(Entity.EntityId);
        if (Entity.Transform.ContainsNaN())
        {
            return Failure(Path + TEXT(".transform"), TEXT("Path node transform must be finite."), true);
        }

        const FAkUGCComponentRecord* PathComponent = nullptr;
        int32 PathComponentCount = 0;
        int32 PathComponentIndex = INDEX_NONE;
        for (int32 ComponentIndex = 0; ComponentIndex < Entity.Components.Num(); ++ComponentIndex)
        {
            if (Entity.Components[ComponentIndex].TypeId == TEXT("tower_defense.path_node"))
            {
                PathComponent = &Entity.Components[ComponentIndex];
                PathComponentIndex = ComponentIndex;
                ++PathComponentCount;
            }
        }
        if (PathComponentCount != 1 || !PathComponent)
        {
            return Failure(
                Path + TEXT(".components"),
                TEXT("Path node must contain exactly one tower_defense.path_node component."),
                true);
        }

        const FString OrderPath = FString::Printf(
            TEXT("%s.components[%d].properties.order"),
            *Path,
            PathComponentIndex);
        const FAkUGCValue* OrderValue = PathComponent->Properties.Find(TEXT("order"));
        if (!OrderValue)
        {
            return Failure(OrderPath, TEXT("Path node order property is required."), true);
        }
        if (OrderValue->Type != EAkUGCValueType::Integer)
        {
            return Failure(OrderPath, TEXT("Path node order must be an Integer value."), true);
        }
        if (OrderValue->IntegerValue < MinimumPathOrder || OrderValue->IntegerValue > MaximumPathOrder)
        {
            return Failure(
                OrderPath,
                FString::Printf(
                    TEXT("Path node order must be between %lld and %lld."),
                    MinimumPathOrder,
                    MaximumPathOrder),
                true);
        }

        FCandidate& Candidate = Candidates.AddDefaulted_GetRef();
        Candidate.EntityIndex = EntityIndex;
        Candidate.OrderPath = OrderPath;
        Candidate.Node.EntityId = Entity.EntityId;
        Candidate.Node.Order = OrderValue->IntegerValue;
        Candidate.Node.Location = Entity.Transform.GetLocation();
    }

    if (Candidates.IsEmpty())
    {
        if (bRequireUsablePath)
        {
            return Failure(TEXT("path"), TEXT("Tower defense path requires at least two path nodes."), false);
        }
        FAkUGCTowerDefensePathBuildResult Result;
        Result.bSucceeded = true;
        return Result;
    }
    if (bRequireUsablePath && Candidates.Num() < 2)
    {
        return Failure(TEXT("path.nodes"), TEXT("Tower defense path requires at least two path nodes."), true);
    }

    Candidates.Sort([](const FCandidate& Left, const FCandidate& Right)
    {
        return Left.Node.Order == Right.Node.Order
            ? GuidLess(Left.Node.EntityId, Right.Node.EntityId)
            : Left.Node.Order < Right.Node.Order;
    });

    for (int32 CandidateIndex = 1; CandidateIndex < Candidates.Num(); ++CandidateIndex)
    {
        const FCandidate& Previous = Candidates[CandidateIndex - 1];
        const FCandidate& Current = Candidates[CandidateIndex];
        if (Previous.Node.Order == Current.Node.Order)
        {
            return Failure(
                Current.OrderPath,
                FString::Printf(TEXT("Path node order %lld is duplicated."), Current.Node.Order),
                true);
        }
        if (FVector::Dist(Previous.Node.Location, Current.Node.Location) < MinimumPathSegmentLength)
        {
            return Failure(
                EntityPath(Current.EntityIndex) + TEXT(".transform.location"),
                FString::Printf(
                    TEXT("Consecutive path nodes must be at least %.0f Unreal unit apart."),
                    MinimumPathSegmentLength),
                true);
        }
    }

    FAkUGCTowerDefensePathBuildResult Result;
    Result.bSucceeded = true;
    Result.bPathPresent = true;
    Result.Path.Nodes.Reserve(Candidates.Num());
    for (const FCandidate& Candidate : Candidates)
    {
        Result.Path.Nodes.Add(Candidate.Node);
    }
    return Result;
}

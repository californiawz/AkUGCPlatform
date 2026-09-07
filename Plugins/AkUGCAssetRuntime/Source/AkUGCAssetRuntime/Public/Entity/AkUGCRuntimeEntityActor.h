#pragma once

#include "GameFramework/Actor.h"
#include "AkUGCRuntimeEntityActor.generated.h"

class UBillboardComponent;
class USceneComponent;
class UStaticMesh;
class UStaticMeshComponent;

UCLASS(NotPlaceable)
class AKUGCASSETRUNTIME_API AAkUGCRuntimeEntityActor : public AActor
{
    GENERATED_BODY()

public:
    AAkUGCRuntimeEntityActor();

    void SetVisualMesh(UStaticMesh* Mesh);

    // 稳定复制标识：权威端设置后随 Actor 复制到客户端，用于跨端关联实体。
    void SetEntityIdentity(const FGuid& InEntityId, const FName& InPrefabId);

    UFUNCTION(BlueprintPure, Category = "UGC|Runtime")
    FGuid GetEntityId() const { return EntityId; }

    UFUNCTION(BlueprintPure, Category = "UGC|Runtime")
    FName GetPrefabId() const { return PrefabId; }

    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "UGC")
    TObjectPtr<USceneComponent> SceneRoot;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "UGC")
    TObjectPtr<UStaticMeshComponent> VisualMesh;

#if WITH_EDITORONLY_DATA
    UPROPERTY(Transient)
    TObjectPtr<UBillboardComponent> EditorSprite;
#endif

private:
    // 稳定复制标识（仅权威端写入，复制到所有客户端）。
    UPROPERTY(Replicated, meta = (AllowPrivateAccess = "true"))
    FGuid EntityId;

    UPROPERTY(Replicated, meta = (AllowPrivateAccess = "true"))
    FName PrefabId;
};

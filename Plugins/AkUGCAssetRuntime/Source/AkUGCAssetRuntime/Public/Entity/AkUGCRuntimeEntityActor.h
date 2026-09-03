#pragma once

#include "GameFramework/Actor.h"
#include "AkUGCRuntimeEntityActor.generated.h"

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

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "UGC")
    TObjectPtr<USceneComponent> SceneRoot;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "UGC")
    TObjectPtr<UStaticMeshComponent> VisualMesh;
};

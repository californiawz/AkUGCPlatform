#include "Entity/AkUGCRuntimeEntityActor.h"

#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"

AAkUGCRuntimeEntityActor::AAkUGCRuntimeEntityActor()
{
    PrimaryActorTick.bCanEverTick = false;
    bReplicates = true;
    SetReplicateMovement(true);

    SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
    SetRootComponent(SceneRoot);

    VisualMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("VisualMesh"));
    VisualMesh->SetupAttachment(SceneRoot);
    VisualMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
}

void AAkUGCRuntimeEntityActor::SetVisualMesh(UStaticMesh* Mesh)
{
    VisualMesh->SetStaticMesh(Mesh);
}

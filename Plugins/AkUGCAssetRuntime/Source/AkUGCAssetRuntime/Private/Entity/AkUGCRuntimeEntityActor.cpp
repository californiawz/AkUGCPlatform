#include "Entity/AkUGCRuntimeEntityActor.h"

#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Net/UnrealNetwork.h"

#if WITH_EDITORONLY_DATA
#include "Components/BillboardComponent.h"
#include "Engine/Texture2D.h"
#include "UObject/ConstructorHelpers.h"
#endif

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

#if WITH_EDITORONLY_DATA
    EditorSprite = CreateEditorOnlyDefaultSubobject<UBillboardComponent>(TEXT("EditorSprite"));
    if (EditorSprite && !IsRunningCommandlet())
    {
        static ConstructorHelpers::FObjectFinderOptional<UTexture2D> SpriteTexture(
            TEXT("/Engine/EditorResources/S_TargetPoint"));
        EditorSprite->Sprite = SpriteTexture.Get();
        EditorSprite->SetRelativeScale3D_Direct(FVector(0.5));
        EditorSprite->bIsScreenSizeScaled = true;
        EditorSprite->SetupAttachment(SceneRoot);
    }
#endif
}

void AAkUGCRuntimeEntityActor::SetVisualMesh(UStaticMesh* Mesh)
{
    VisualMesh->SetStaticMesh(Mesh);
#if WITH_EDITORONLY_DATA
    if (EditorSprite)
    {
        EditorSprite->SetVisibility(Mesh == nullptr);
    }
#endif
}

void AAkUGCRuntimeEntityActor::SetEntityIdentity(const FGuid& InEntityId, const FName& InPrefabId)
{
    EntityId = InEntityId;
    PrefabId = InPrefabId;
}

void AAkUGCRuntimeEntityActor::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);

    DOREPLIFETIME(AAkUGCRuntimeEntityActor, EntityId);
    DOREPLIFETIME(AAkUGCRuntimeEntityActor, PrefabId);
}

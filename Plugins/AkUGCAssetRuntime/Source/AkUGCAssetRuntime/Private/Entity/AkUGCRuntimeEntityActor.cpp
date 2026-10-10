#include "Entity/AkUGCRuntimeEntityActor.h"

#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Net/UnrealNetwork.h"
#include "Engine/StaticMesh.h"
#include "Prefab/AkUGCOfficialPrefabCatalog.h"
#include "Prefab/AkUGCPrefabRegistry.h"
#include "Scene/AkUGCSceneRuntime.h"

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

void AAkUGCRuntimeEntityActor::BeginPlay()
{
    Super::BeginPlay();
    if (GetNetMode() == NM_Client) { OnRep_PrefabId(); }
}

void AAkUGCRuntimeEntityActor::OnRep_PrefabId()
{
#if !UE_SERVER
    SetVisualMesh(nullptr);
    if (GetNetMode() == NM_DedicatedServer || PrefabId.IsNone()) { return; }
    static const FAkUGCPrefabRegistry Registry = []()
    {
        FAkUGCPrefabRegistry Result;
        FAkUGCOfficialPrefabCatalog::RegisterTowerDefense(Result);
        return Result;
    }();
    const FSoftObjectPath* Path = Registry.ResolveAsset(PrefabId, FAkUGCSceneRuntime::GetCurrentPlatformVariant());
    if (!Path || !Path->IsValid()) { return; }
    UStaticMesh* Mesh = Cast<UStaticMesh>(Path->TryLoad());
    if (Mesh) { SetVisualMesh(Mesh); }
    else { UE_LOG(LogTemp, Warning, TEXT("Failed to resolve replicated official Prefab visual: %s"), *PrefabId.ToString()); }
#endif
}

void AAkUGCRuntimeEntityActor::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);

    DOREPLIFETIME(AAkUGCRuntimeEntityActor, EntityId);
    DOREPLIFETIME(AAkUGCRuntimeEntityActor, PrefabId);
}

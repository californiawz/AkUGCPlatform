#include "Game/AkUGCPlayerController.h"

#include "Camera/CameraActor.h"
#include "Components/InputComponent.h"
#include "Engine/World.h"
#include "Game/AkUGCGameMode.h"
#include "InputCoreTypes.h"

void AAkUGCPlayerController::SetupInputComponent()
{
    Super::SetupInputComponent();
    if (InputComponent)
    {
        InputComponent->BindKey(EKeys::Enter, IE_Pressed, this, &AAkUGCPlayerController::StartSolo);
        InputComponent->BindKey(EKeys::Escape, IE_Pressed, this, &AAkUGCPlayerController::ReturnSolo);
    }
}

void AAkUGCPlayerController::BeginPlay()
{
    Super::BeginPlay();
    if (IsLocalController() && GetWorld()->GetNetMode() == NM_Standalone)
    {
        ACameraActor* Camera = GetWorld()->SpawnActor<ACameraActor>();
        if (Camera)
        {
            Camera->SetActorLocation(FVector(750.0, 0.0, 1800.0));
            Camera->SetActorRotation(FRotator(-90.0, 0.0, 0.0));
            SetViewTarget(Camera);
        }
    }
}

void AAkUGCPlayerController::StartSolo()
{
    if (!IsLocalController() || !HasAuthority()) { return; }
    AAkUGCGameMode* Mode = GetWorld()->GetAuthGameMode<AAkUGCGameMode>();
    FString Error;
    if (Mode && !Mode->StartSoloMatch(&Error)) { UE_LOG(LogTemp, Warning, TEXT("Start solo rejected: %s"), *Error); }
}

void AAkUGCPlayerController::ReturnSolo()
{
    if (!IsLocalController() || !HasAuthority()) { return; }
    AAkUGCGameMode* Mode = GetWorld()->GetAuthGameMode<AAkUGCGameMode>();
    FString Error;
    if (Mode && !Mode->ReturnToPreparation(&Error)) { UE_LOG(LogTemp, Warning, TEXT("Return solo rejected: %s"), *Error); }
}

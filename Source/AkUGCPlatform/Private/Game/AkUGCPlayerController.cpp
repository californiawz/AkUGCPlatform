#include "Game/AkUGCPlayerController.h"

#include "Camera/CameraActor.h"
#include "Components/InputComponent.h"
#include "Engine/World.h"
#include "Game/AkUGCGameMode.h"
#include "InputCoreTypes.h"
#include "Game/AkUGCGameState.h"
#include "Game/AkUGCRoom.h"
#include "Kismet/GameplayStatics.h"
#include "Net/UnrealNetwork.h"

void AAkUGCPlayerController::SetupInputComponent()
{
    Super::SetupInputComponent();
    if (InputComponent)
    {
        InputComponent->BindKey(EKeys::Enter, IE_Pressed, this, &AAkUGCPlayerController::StartOrReady);
        InputComponent->BindKey(EKeys::Escape, IE_Pressed, this, &AAkUGCPlayerController::ReturnOrLeave);
        InputComponent->BindKey(EKeys::F5, IE_Pressed, this, &AAkUGCPlayerController::StartRoom);
        InputComponent->BindKey(EKeys::F6, IE_Pressed, this, &AAkUGCPlayerController::HostRoom);
        InputComponent->BindKey(EKeys::F7, IE_Pressed, this, &AAkUGCPlayerController::NextWork);
        InputComponent->BindKey(EKeys::F8, IE_Pressed, this, &AAkUGCPlayerController::PlaySelectedWork);
    }
}

void AAkUGCPlayerController::BeginPlay()
{
    Super::BeginPlay();
    if (IsLocalController() && GetWorld()->GetNetMode() != NM_DedicatedServer)
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

void AAkUGCPlayerController::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(AAkUGCPlayerController, RoomMemberId);
}

void AAkUGCPlayerController::HostRoom()
{
#if !UE_SERVER && !UE_CLIENT
    if (IsLocalController() && GetNetMode() == NM_Standalone)
    {
        UGameplayStatics::OpenLevel(this, TEXT("/Game/Maps/Phase0"), true, TEXT("listen?room=1"));
    }
#endif
}

void AAkUGCPlayerController::JoinRoom(const FString& Endpoint)
{
    FString Canonical;
    if (IsLocalController() && GetNetMode() == NM_Standalone && FAkUGCRoomRoster::ValidateEndpoint(Endpoint, Canonical))
    {
        ClientTravel(Canonical, TRAVEL_Absolute);
    }
}

void AAkUGCPlayerController::LeaveRoom()
{
    if (IsLocalController() && GetNetMode() != NM_Standalone)
    {
        UGameplayStatics::OpenLevel(this, TEXT("/Game/Maps/Phase0"), true, TEXT("local=1"));
    }
}

void AAkUGCPlayerController::ReadyRoom()
{
    if (!IsLocalController()) { return; }
    const AAkUGCGameState* State = GetWorld()->GetGameState<AAkUGCGameState>();
    if (!State) { return; }
    for (const auto& Member : State->GetRoomMembers())
    {
        if (Member.MemberId == RoomMemberId) { ServerSetRoomReady(!Member.bReady); break; }
    }
}

void AAkUGCPlayerController::StartRoom() { if (IsLocalController()) { ServerStartRoom(); } }
void AAkUGCPlayerController::ReturnRoom() { if (IsLocalController()) { ServerReturnRoom(); } }

void AAkUGCPlayerController::ServerSetRoomReady_Implementation(bool bReady)
{
    if (AAkUGCGameMode* Mode = GetWorld()->GetAuthGameMode<AAkUGCGameMode>()) { Mode->SetMemberReady(this, bReady); }
}
void AAkUGCPlayerController::ServerStartRoom_Implementation()
{
    if (AAkUGCGameMode* Mode = GetWorld()->GetAuthGameMode<AAkUGCGameMode>()) { Mode->StartRoomMatch(this); }
}
void AAkUGCPlayerController::ServerReturnRoom_Implementation()
{
    if (AAkUGCGameMode* Mode = GetWorld()->GetAuthGameMode<AAkUGCGameMode>()) { Mode->ReturnRoomToPreparation(this); }
}

void AAkUGCPlayerController::StartOrReady()
{
    if (RoomMemberId > 0) { ReadyRoom(); } else { StartSolo(); }
}
void AAkUGCPlayerController::ReturnOrLeave()
{
    if (RoomMemberId > 0)
    {
        const auto* State = GetWorld()->GetGameState<AAkUGCGameState>();
        const bool bHost = State && State->GetRoomMembers().ContainsByPredicate([&](const FAkUGCRoomMember& Member)
        { return Member.MemberId == RoomMemberId && Member.bHost; });
        if (bHost && State->GetMatchFlow() == EAkUGCMatchFlow::Settlement) { ReturnRoom(); } else { LeaveRoom(); }
    }
    else { ReturnSolo(); }
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

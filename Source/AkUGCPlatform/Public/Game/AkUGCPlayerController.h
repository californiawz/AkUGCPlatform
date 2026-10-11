#pragma once

#include "GameFramework/PlayerController.h"
#include "AkUGCPlayerController.generated.h"

UCLASS()
class AKUGCPLATFORM_API AAkUGCPlayerController : public APlayerController
{
    GENERATED_BODY()

public:
    virtual void BeginPlay() override;
    virtual void SetupInputComponent() override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
    void SetRoomMemberId(int32 Id) { if (HasAuthority()) { RoomMemberId = Id; } }
    int32 GetRoomMemberId() const { return RoomMemberId; }

    UFUNCTION(Exec)
    void HostRoom();
    UFUNCTION(Exec)
    void JoinRoom(const FString& Endpoint);
    UFUNCTION(Exec)
    void LeaveRoom();
    UFUNCTION(Exec)
    void ReadyRoom();
    UFUNCTION(Exec)
    void StartRoom();
    UFUNCTION(Exec)
    void ReturnRoom();

    UFUNCTION(Server, Reliable)
    void ServerSetRoomReady(bool bReady);
    UFUNCTION(Server, Reliable)
    void ServerStartRoom();
    UFUNCTION(Server, Reliable)
    void ServerReturnRoom();

    UFUNCTION(Exec)
    void StartSolo();

    UFUNCTION(Exec)
    void ReturnSolo();

private:
    UPROPERTY(Replicated)
    int32 RoomMemberId = 0;
    void StartOrReady();
    void ReturnOrLeave();
};

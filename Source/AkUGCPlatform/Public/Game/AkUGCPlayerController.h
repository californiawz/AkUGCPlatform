#pragma once

#include "GameFramework/PlayerController.h"
#include "AkUGCPlayerController.generated.h"

UCLASS()
class AKUGCPLATFORM_API AAkUGCPlayerController : public APlayerController
{
    GENERATED_BODY()

public:
    AAkUGCPlayerController();
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

    UFUNCTION(Exec)
    void ListWorks();
    UFUNCTION(Exec)
    void ImportWork(const FString& SharePath, const FString& IndependentlyTrustedPublicKey);
    UFUNCTION(Exec)
    void ExportWork(const FString& ReleaseId, const FString& Destination);
    UFUNCTION(Exec)
    void DeleteWork(const FString& ReleaseId);
    UFUNCTION(Exec)
    void PlayWork(const FString& ReleaseId);
    void NextWork();
    void PlaySelectedWork();
    FString GetWorkLibraryStatus() const { return WorkLibraryStatus; }
    static FString WorkLibraryDirectory();

private:
    FString SelectedWorkId;
    FString WorkLibraryStatus;
    UPROPERTY(Replicated)
    int32 RoomMemberId = 0;
    void StartOrReady();
    void ReturnOrLeave();
};

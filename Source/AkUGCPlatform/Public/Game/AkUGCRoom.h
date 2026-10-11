#pragma once

#include "CoreMinimal.h"
#include "AkUGCRoom.generated.h"

USTRUCT(BlueprintType)
struct AKUGCPLATFORM_API FAkUGCRoomMember
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Room")
    int32 MemberId = 0;

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Room")
    bool bReady = false;

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Room")
    bool bHost = false;
};

class AKUGCPLATFORM_API FAkUGCRoomRoster
{
public:
    static constexpr int32 MaxMembers = 4;
    int32 Join(bool bPreferHost = false);
    void Leave(int32 MemberId);
    bool SetReady(int32 MemberId, bool bReady);
    bool CanStart(int32 Requester) const;
    bool IsHost(int32 MemberId) const;
    void ResetReady();
    const TArray<FAkUGCRoomMember>& GetMembers() const { return Members; }
    static bool ValidateEndpoint(const FString& Input, FString& OutEndpoint);

private:
    int32 NextMemberId = 1;
    TArray<FAkUGCRoomMember> Members;
};

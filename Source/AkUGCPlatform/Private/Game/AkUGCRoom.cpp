#include "Game/AkUGCRoom.h"

int32 FAkUGCRoomRoster::Join(bool bPreferHost)
{
    if (Members.Num() >= MaxMembers || NextMemberId == MAX_int32) { return 0; }
    FAkUGCRoomMember Member;
    Member.MemberId = NextMemberId++;
    Member.bHost = Members.IsEmpty() || bPreferHost;
    if (bPreferHost) { for (auto& Existing : Members) { Existing.bHost = false; } }
    Members.Add(Member);
    return Member.MemberId;
}

void FAkUGCRoomRoster::Leave(int32 MemberId)
{
    const bool bWasHost = IsHost(MemberId);
    Members.RemoveAll([&](const FAkUGCRoomMember& Member) { return Member.MemberId == MemberId; });
    if (bWasHost && !Members.IsEmpty()) { Members[0].bHost = true; }
}

bool FAkUGCRoomRoster::SetReady(int32 MemberId, bool bReady)
{
    for (auto& Member : Members)
    {
        if (Member.MemberId == MemberId) { Member.bReady = bReady; return true; }
    }
    return false;
}

bool FAkUGCRoomRoster::IsHost(int32 MemberId) const
{
    return Members.ContainsByPredicate([&](const FAkUGCRoomMember& Member) { return Member.MemberId == MemberId && Member.bHost; });
}

bool FAkUGCRoomRoster::CanStart(int32 Requester) const
{
    return Members.Num() >= 2 && Members.Num() <= MaxMembers && IsHost(Requester)
        && !Members.ContainsByPredicate([](const FAkUGCRoomMember& Member) { return !Member.bReady; });
}

void FAkUGCRoomRoster::ResetReady()
{
    for (auto& Member : Members) { Member.bReady = false; }
}

bool FAkUGCRoomRoster::ValidateEndpoint(const FString& Input, FString& OutEndpoint)
{
    OutEndpoint.Reset();
    if (Input.Len() < 9 || Input.Len() > 21) { return false; }
    FString Address, Port;
    if (!Input.Split(TEXT(":"), &Address, &Port)) { return false; }
    const auto Parse = [](const FString& Value, int32 Maximum, int32& Out)
    {
        if (Value.IsEmpty() || Value.Len() > 5) { return false; }
        Out = 0;
        for (TCHAR Character : Value)
        {
            if (Character < TEXT('0') || Character > TEXT('9')) { return false; }
            Out = Out * 10 + Character - TEXT('0');
            if (Out > Maximum) { return false; }
        }
        return true;
    };
    TArray<FString> Parts;
    Address.ParseIntoArray(Parts, TEXT("."), false);
    if (Parts.Num() != 4) { return false; }
    int32 Octets[4], Number;
    for (int32 Index = 0; Index < 4; ++Index)
    {
        if (Parts[Index].Len() > 3 || !Parse(Parts[Index], 255, Octets[Index])) { return false; }
    }
    if (!Parse(Port, 65535, Number) || Number == 0 || Octets[0] == 0 || Octets[0] >= 224
        || (Octets[0] == 255 && Octets[1] == 255 && Octets[2] == 255 && Octets[3] == 255)) { return false; }
    OutEndpoint = FString::Printf(TEXT("%d.%d.%d.%d:%d"), Octets[0], Octets[1], Octets[2], Octets[3], Number);
    return true;
}

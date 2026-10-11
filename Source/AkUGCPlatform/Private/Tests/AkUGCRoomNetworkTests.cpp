#include "Misc/AutomationTest.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Entity/AkUGCRuntimeEntityActor.h"
#include "Game/AkUGCGameState.h"
#include "Game/AkUGCPlayerController.h"
#include "Game/AkUGCGameMode.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AkUGCRoomNetwork
{
    class FObserve final : public IAutomationLatentCommand
    {
    public:
        explicit FObserve(FAutomationTestBase* InTest) : Test(InTest), Start(FPlatformTime::Seconds()) {}
        bool Update() override
        {
            if (FPlatformTime::Seconds() - Start > 160.0) { Test->AddError(TEXT("Room real-network test timed out.")); return true; }
            UWorld* World = nullptr;
            for (const auto& Context : GEngine->GetWorldContexts())
            {
                UWorld* Candidate = Context.World();
                if (Candidate && Candidate->IsGameWorld() && (Candidate->GetNetMode() == NM_Client || Candidate->GetNetMode() == NM_ListenServer))
                { World = Candidate; break; }
            }
            if (!World) { return false; }
            auto* State = World->GetGameState<AAkUGCGameState>();
            auto* Player = Cast<AAkUGCPlayerController>(World->GetFirstPlayerController());
            if (!State || !Player || Player->GetRoomMemberId() <= 0) { return false; }
            const auto& Members = State->GetRoomMembers();
            const auto* Self = Members.FindByPredicate([&](const FAkUGCRoomMember& Member) { return Member.MemberId == Player->GetRoomMemberId(); });
            if (!Self) { return false; }
            if (!bJoined)
            {
                bJoined = true;
                UE_LOG(LogTemp, Display, TEXT("[AkUGCRoomTest] Joined Id=%d Host=%d Members=%d Flow=%d"), Self->MemberId, Self->bHost, Members.Num(), static_cast<int32>(State->GetMatchFlow()));
                if (!Self->bHost) { Player->ServerStartRoom(); }
            }
            if (State->GetMatchFlow() == EAkUGCMatchFlow::Preparation)
            {
                if (CompletedRound > 0) { bReadySent = false; bStartSent = false; bReturnSent = false; CompletedRound = 0; }
                int32 ExpectedMembers = 4;
                FParse::Value(FCommandLine::Get(), TEXT("RoomExpectedMembers="), ExpectedMembers);
                if (Members.Num() < ExpectedMembers) { return false; }
                if (!bReadySent) { Player->ServerSetRoomReady(true); bReadySent = true; }
                if (Self->bHost && !bStartSent && Members.Num() >= 2
                    && !Members.ContainsByPredicate([](const FAkUGCRoomMember& Member) { return !Member.bReady; }))
                {
                    Player->ServerStartRoom(); bStartSent = true;
                }
                return false;
            }
            for (TActorIterator<AAkUGCRuntimeEntityActor> It(World); It; ++It)
            {
                if (!bEnemy && It->GetPrefabId() == TEXT("official.unit.basic_enemy"))
                {
                    bEnemy = true;
                    UE_LOG(LogTemp, Display, TEXT("[AkUGCRoomTest] EnemyObserved Members=%d Round=%d"), Members.Num(), State->GetRoundId());
                }
            }
            if (FParse::Param(FCommandLine::Get(), TEXT("RoomTestDisconnect")) && Members.Num() == 4)
            {
                if (DisconnectAt == 0.0) { DisconnectAt = FPlatformTime::Seconds() + 6.0; }
                if (FPlatformTime::Seconds() >= DisconnectAt)
                {
                    const int32 OldId = Self->MemberId;
                    Player->LeaveRoom();
                    UE_LOG(LogTemp, Display, TEXT("[AkUGCRoomTest] LeftForReconnect Id=%d"), OldId);
                    return true;
                }
                return false;
            }
            const auto Score = State->GetScoreSnapshot();
            if (!Score.bSettled || Score.Result != EAkUGCTowerDefenseMatchResult::Victory || Score.RoundId != State->GetRoundId()) { return false; }
            if (Score.TeamScore != 500 || Score.EnemyKills != 0 || Score.Resources != 321 || Score.FinalBaseMaximumHealth != 1000.0)
            {
                Test->AddError(TEXT("Room settlement differs from authoritative fixture.")); return true;
            }
            if (LastObservedRound != Score.RoundId)
            {
                LastObservedRound = Score.RoundId;
                ++RoundsObserved;
                UE_LOG(LogTemp, Display, TEXT("[AkUGCRoomTest] Settlement Round=%d Score=%d Resources=%d Kills=%d Base=%.0f Members=%d"),
                    Score.RoundId, Score.TeamScore, Score.Resources, Score.EnemyKills, Score.FinalBaseCurrentHealth, Members.Num());
            }
            int32 TargetRounds = 1;
            FParse::Value(FCommandLine::Get(), TEXT("RoomTestRounds="), TargetRounds);
            if (RoundsObserved < TargetRounds)
            {
                CompletedRound = Score.RoundId;
                if (Self->bHost && !bReturnSent && State->GetMatchFlow() == EAkUGCMatchFlow::Settlement)
                {
                    if (ReturnAt == 0.0) { ReturnAt = FPlatformTime::Seconds() + 2.0; }
                    if (FPlatformTime::Seconds() >= ReturnAt) { Player->ServerReturnRoom(); bReturnSent = true; ReturnAt = 0.0; }
                }
                return false;
            }
            UE_LOG(LogTemp, Display, TEXT("[AkUGCRoomTest] Passed Rounds=%d Round=%d Score=%d Resources=%d Kills=%d Base=%.0f Members=%d EnemyObserved=%d"),
                RoundsObserved, Score.RoundId, Score.TeamScore, Score.Resources, Score.EnemyKills, Score.FinalBaseCurrentHealth, Members.Num(), bEnemy);
            return true;
        }
    private:
        FAutomationTestBase* Test;
        double Start;
        double DisconnectAt = 0.0;
        double ReturnAt = 0.0;
        bool bJoined = false, bReadySent = false, bStartSent = false, bEnemy = false, bReturnSent = false;
        int32 CompletedRound = 0, LastObservedRound = INDEX_NONE, RoundsObserved = 0;
    };
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAkUGCRoomNetworkClientTest, "AkUGC.Runtime.Room.RealNetwork",
    EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FAkUGCRoomNetworkClientTest::RunTest(const FString& Parameters)
{
    if (!FParse::Param(FCommandLine::Get(), TEXT("AkUGCRoomTest"))) { AddWarning(TEXT("Requires actual room network and -AkUGCRoomTest.")); return true; }
    ADD_LATENT_AUTOMATION_COMMAND(AkUGCRoomNetwork::FObserve(this));
    return true;
}
#endif

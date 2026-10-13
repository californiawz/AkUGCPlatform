#include "Misc/AutomationTest.h"
#include "AkUGCSceneVisualEvidence.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/NetDriver.h"
#include "EngineUtils.h"
#include "Entity/AkUGCRuntimeEntityActor.h"
#include "Game/AkUGCGameState.h"
#include "Game/AkUGCPlayerController.h"
#include "Game/AkUGCGameMode.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Engine/Console.h"
#include "Engine/GameViewportClient.h"

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
namespace AkUGCRoomRecovery
{
    class FObserve final : public IAutomationLatentCommand
    {
    public:
        explicit FObserve(FAutomationTestBase* InTest) : Test(InTest), Started(FPlatformTime::Seconds()) {}
        bool Update() override
        {
            if (FPlatformTime::Seconds() - Started > 115) { Test->AddError(TEXT("Room recovery timed out.")); return true; }
            UWorld* World = nullptr;
            for (const auto& Context : GEngine->GetWorldContexts())
            { if (Context.World() && Context.World()->IsGameWorld()) { World = Context.World(); break; } }
            if (!World) { return false; }
            auto* State = World->GetGameState<AAkUGCGameState>();
            auto* Player = Cast<AAkUGCPlayerController>(World->GetFirstPlayerController());
            if (!State || !Player) { return false; }
            if (!bAnnounced)
            {
                bAnnounced = true;
                UE_LOG(LogTemp, Display, TEXT("[AkUGCRoomRecovery] Target Editor=%d Client=%d Server=%d Saved=%s"),
                    WITH_EDITOR, UE_CLIENT, UE_SERVER, *FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()));
            }
            if (!bJoined)
            {
                if (World->GetNetMode() != NM_Client && World->GetNetMode() != NM_ListenServer) { return false; }
                if (Player->GetRoomMemberId() <= 0 || State->GetRoomMembers().Num() < 2) { return false; }
                bJoined = true;
                FaultAt = FPlatformTime::Seconds();
                UE_LOG(LogTemp, Display, TEXT("[AkUGCRoomRecovery] Joined Members=%d NetMode=%d"), State->GetRoomMembers().Num(), World->GetNetMode());
                FString Fault;
                FParse::Value(FCommandLine::Get(), TEXT("RoomRecoveryFault="), Fault);
                if (World->GetNetMode() == NM_Client)
                {
                    if (Fault == TEXT("leave"))
                    { Test->AddExpectedError(TEXT("UEngine::BroadcastNetworkFailure: FailureType = FailureReceived"), EAutomationExpectedErrorFlags::Contains, 1); }
                    Test->AddExpectedError(Fault == TEXT("leave") ? TEXT("UEngine::BroadcastNetworkFailure: FailureType = ConnectionLost")
                        : TEXT("UEngine::BroadcastNetworkFailure: FailureType = ConnectionTimeout"), EAutomationExpectedErrorFlags::Contains, 1);
                    FString Ready;
                    if (!FParse::Value(FCommandLine::Get(), TEXT("RoomRecoveryReady="), Ready)
                        || !FPaths::IsUnderDirectory(FPaths::ConvertRelativePathToFull(Ready), FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()))
                        || !FFileHelper::SaveStringToFile(TEXT("joined"), *Ready))
                    { Test->AddError(TEXT("Cannot signal joined receiver readiness.")); return true; }
                }
                if (World->GetNetMode() == NM_Client && Fault == TEXT("timeout"))
                {
#if DO_ENABLE_NET_TEST
                    UNetDriver* Driver = World->GetNetDriver();
                    if (!Driver) { Test->AddError(TEXT("Missing net driver for silent packet loss.")); return true; }
                    Driver->ConnectionTimeout = 4;
                    FPacketSimulationSettings Settings;
                    Settings.PktLoss = 100;
                    Settings.PktIncomingLoss = 100;
                    Driver->SetPacketSimulationSettings(Settings);
                    UE_LOG(LogTemp, Display, TEXT("[AkUGCRoomRecovery] BlackholeApplied IncomingLoss=100 OutgoingLoss=100 ConnectionTimeout=4"));
#else
                    Test->AddError(TEXT("Network simulation unavailable.")); return true;
#endif
                }
                return false;
            }
            if (World->GetNetMode() == NM_ListenServer && !bLeaveIssued && FPlatformTime::Seconds() - FaultAt >= 5)
            {
                FString Fault;
                FParse::Value(FCommandLine::Get(), TEXT("RoomRecoveryFault="), Fault);
                FString Ready;
                if (Fault == TEXT("leave") && FParse::Value(FCommandLine::Get(), TEXT("RoomRecoveryReady="), Ready)
                    && IFileManager::Get().FileExists(*Ready))
                {
                    bLeaveIssued = true;
                    Player->LeaveRoom();
                    UE_LOG(LogTemp, Display, TEXT("[AkUGCRoomRecovery] HostLeaveIssued"));
                }
                return false;
            }
            if (World->GetNetMode() != NM_Standalone) { return false; }
            auto* Mode = World->GetAuthGameMode<AAkUGCGameMode>();
            if (!Mode) { return false; }
            if (Mode->IsRoomMode() || Mode->HasAuthoritySession() || !Mode->HasPreparedProject()
                || Player->GetRoomMemberId() != 0 || !State->GetRoomMembers().IsEmpty()
                || State->GetMatchFlow() != EAkUGCMatchFlow::Preparation || World->GetNetDriver())
            { Test->AddError(TEXT("Disconnected client did not return to clean local preparation.")); return true; }
            FString Error;
            if (!Mode->StartSoloMatch(&Error) || !Mode->HasAuthoritySession() || !Mode->ReturnToPreparation(&Error)
                || Mode->HasAuthoritySession() || State->GetMatchFlow() != EAkUGCMatchFlow::Preparation)
            { Test->AddError(TEXT("Recovered local preparation is not operable: ") + Error); return true; }
            UE_LOG(LogTemp, Display, TEXT("[AkUGCRoomRecovery] Passed CleanLocal=1 SoloStartReturn=1 Elapsed=%.3f"), FPlatformTime::Seconds() - FaultAt);
            return true;
        }
    private:
        FAutomationTestBase* Test;
        double Started, FaultAt = 0;
        bool bJoined = false, bLeaveIssued = false, bAnnounced = false;
    };
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAkUGCRoomRecoveryTest, "AkUGC.Runtime.Room.FaultRecovery",
    EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FAkUGCRoomRecoveryTest::RunTest(const FString& Parameters)
{
    if (!FParse::Param(FCommandLine::Get(), TEXT("AkUGCRoomRecovery")))
    { AddWarning(TEXT("Room recovery skipped: explicit isolated failure driver required.")); return true; }
    ADD_LATENT_AUTOMATION_COMMAND(AkUGCRoomRecovery::FObserve(this));
    return true;
}
namespace AkUGCRoomKeyboard
{
    class FObserve final : public IAutomationLatentCommand
    {
    public:
        FObserve(FAutomationTestBase* InTest, FString InOutput) : Test(InTest), Output(MoveTemp(InOutput)), Started(FPlatformTime::Seconds()) {}
        bool Update() override
        {
            if (FPlatformTime::Seconds() - Started > 300) { if (Visual) { Visual->Cancel(); } Test->AddError(TEXT("Room keyboard verification timed out.")); return true; }
            if (FPlatformTime::Seconds() - LastPoll < 0.1) { return false; }
            LastPoll = FPlatformTime::Seconds();
            UWorld* World = nullptr;
            for (const auto& Context : GEngine->GetWorldContexts())
            { if (Context.World() && Context.World()->IsGameWorld()) { World = Context.World(); break; } }
            if (!World) { return false; }
            auto* State = World->GetGameState<AAkUGCGameState>();
            auto* Player = Cast<AAkUGCPlayerController>(World->GetFirstPlayerController());
            if (!State || !Player) { return false; }
            const auto Score = State->GetScoreSnapshot();
            const auto& Members = State->GetRoomMembers();
            const auto* Self = Members.FindByPredicate([&](const FAkUGCRoomMember& M) { return M.MemberId == Player->GetRoomMemberId(); });
            const bool AllReady = Members.Num() == 2 && !Members.ContainsByPredicate([](const FAkUGCRoomMember& M) { return !M.bReady; });
            const bool NoneReady = !Members.ContainsByPredicate([](const FAkUGCRoomMember& M) { return M.bReady; });
            const auto Data = MakeShared<FJsonObject>();
            Data->SetNumberField(TEXT("editor"), WITH_EDITOR); Data->SetNumberField(TEXT("client"), UE_CLIENT);
            Data->SetBoolField(TEXT("console_open"), World->GetGameViewport() && World->GetGameViewport()->ViewportConsole
                && !World->GetGameViewport()->ViewportConsole->ConsoleState.IsNone());
            Data->SetNumberField(TEXT("net_mode"), World->GetNetMode());
            Data->SetNumberField(TEXT("members"), Members.Num()); Data->SetNumberField(TEXT("member_id"), Player->GetRoomMemberId());
            Data->SetBoolField(TEXT("host"), Self && Self->bHost); Data->SetBoolField(TEXT("all_ready"), AllReady);
            Data->SetBoolField(TEXT("none_ready"), NoneReady); Data->SetNumberField(TEXT("flow"), static_cast<int32>(State->GetMatchFlow()));
            Data->SetNumberField(TEXT("round"), State->GetRoundId()); Data->SetNumberField(TEXT("score_round"), Score.RoundId);
            Data->SetNumberField(TEXT("score"), Score.TeamScore); Data->SetNumberField(TEXT("resources"), Score.Resources);
            Data->SetNumberField(TEXT("kills"), Score.EnemyKills); Data->SetBoolField(TEXT("settled"), Score.bSettled);
            Data->SetNumberField(TEXT("result"), static_cast<int32>(Score.Result));
            Data->SetNumberField(TEXT("base"), Score.FinalBaseCurrentHealth); Data->SetNumberField(TEXT("base_max"), Score.FinalBaseMaximumHealth);
            Data->SetNumberField(TEXT("active_enemies"), State->GetActiveEnemyCount());
            auto* Mode = World->GetAuthGameMode<AAkUGCGameMode>();
            Data->SetBoolField(TEXT("prepared"), Mode && Mode->HasPreparedProject());
            Data->SetBoolField(TEXT("authority_session"), Mode && Mode->HasAuthoritySession());
            Data->SetObjectField(TEXT("scene"), AkUGCSceneVisual::Snapshot(World));
            Save(Data, TEXT("state.json"));
            FString Json; TSharedPtr<FJsonObject> Command;
            if (!FFileHelper::LoadFileToString(Json, *FPaths::Combine(Output, TEXT("command.json")))
                || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Command) || !Command.IsValid()) { return false; }
            double Seq = 0; FString Tag;
            if (!Command->TryGetNumberField(TEXT("seq"), Seq) || Seq <= LastSeq || !Command->TryGetStringField(TEXT("tag"), Tag)) { return false; }
            const bool SampleCheckpoint = Tag == TEXT("playing1") || Tag == TEXT("playing2");
            FString SceneOutput;
            if (SampleCheckpoint && FParse::Value(FCommandLine::Get(), TEXT("AkUGCSceneVisual="), SceneOutput))
            {
                if (!Visual) { Visual = MakeUnique<AkUGCSceneVisual::FCapture>(Tag); }
                if (!Visual->Update(World)) { return false; }
                if (!Visual->Succeeded()) { Test->AddError(Visual->GetError()); Visual->Cancel(); return true; }
                Data->SetStringField(TEXT("scene_report"), Visual->GetReportPath());
                Visual.Reset();
            }
            LastSeq = Seq;
            const TArray<FString> Order{TEXT("local"), TEXT("joined"), TEXT("unready"), TEXT("ready1"), TEXT("nonhost"),
                TEXT("playing1"), TEXT("settled1"), TEXT("returned1"), TEXT("ready2"), TEXT("playing2"), TEXT("settled2"), TEXT("returned2")};
            bool Passed = false;
            if (Tag == TEXT("finish"))
            {
                Passed = Index == Order.Num() && !Failed;
                Data->SetBoolField(TEXT("passed"), Passed); Data->SetBoolField(TEXT("human_ui_validated"), false);
                Data->SetArrayField(TEXT("checkpoints"), Checkpoints); Save(Data, TEXT("result.json"));
                if (!Passed) { Test->AddError(TEXT("Room keyboard checkpoints failed.")); }
                return true;
            }
            const bool Room = Self && Members.Num() == 2
                && (World->GetNetMode() == NM_Client || World->GetNetMode() == NM_ListenServer);
            const bool Preparation = State->GetMatchFlow() == EAkUGCMatchFlow::Preparation && !Score.bSettled
                && Score.TeamScore == 0 && (!Mode || !Mode->HasAuthoritySession());
            if (Tag == TEXT("local"))
            { Passed = World->GetNetMode() == NM_Standalone && !World->GetNetDriver() && Members.IsEmpty()
                && Player->GetRoomMemberId() == 0 && Mode && Mode->HasPreparedProject() && !Mode->IsRoomMode() && Preparation; }
            if (Tag == TEXT("joined") || Tag == TEXT("unready")) { Passed = Room && Preparation && NoneReady; }
            if (Tag == TEXT("ready1") || Tag == TEXT("nonhost") || Tag == TEXT("ready2")) { Passed = Room && Preparation && AllReady; }
            if (Tag == TEXT("playing1") || Tag == TEXT("playing2"))
            {
                Passed = Room && State->GetMatchFlow() == EAkUGCMatchFlow::Playing && !Score.bSettled
                    && (!Mode || Mode->HasAuthoritySession());
                if (Tag == TEXT("playing1")) { FirstRound = State->GetRoundId(); }
                else { Passed &= State->GetRoundId() > FirstRound; SecondRound = State->GetRoundId(); }
            }
            if (Tag == TEXT("settled1") || Tag == TEXT("settled2"))
            {
                Passed = Room && State->GetMatchFlow() == EAkUGCMatchFlow::Settlement && Score.bSettled
                    && Score.RoundId == State->GetRoundId() && State->GetActiveEnemyCount() == 0
                    && Score.Result == EAkUGCTowerDefenseMatchResult::Victory && Score.TeamScore > 0;
                Passed &= State->GetRoundId() == (Tag == TEXT("settled1") ? FirstRound : SecondRound);
            }
            if (Tag == TEXT("returned1") || Tag == TEXT("returned2")) { Passed = Room && Preparation && NoneReady
                && State->GetRoundId() > (Tag == TEXT("returned1") ? FirstRound : SecondRound); }
            Passed &= Index < Order.Num() && Order[Index] == Tag;
            if (Index < Order.Num()) { ++Index; }
            Failed |= !Passed;
            Data->SetStringField(TEXT("tag"), Tag); Data->SetBoolField(TEXT("passed"), Passed);
            Checkpoints.Add(MakeShared<FJsonValueObject>(Data));
            const auto Ack = MakeShared<FJsonObject>(); Ack->SetNumberField(TEXT("seq"), Seq); Ack->SetBoolField(TEXT("passed"), Passed);
            Save(Ack, TEXT("ack.json"));
            return false;
        }
    private:
        void Save(const TSharedRef<FJsonObject>& Data, const FString& Name)
        {
            FString Json; FJsonSerializer::Serialize(Data, TJsonWriterFactory<>::Create(&Json));
            FFileHelper::SaveStringToFile(Json, *FPaths::Combine(Output, Name), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
        }
        FAutomationTestBase* Test;
        FString Output;
        double Started, LastPoll = 0, LastSeq = 0;
        int32 Index = 0, FirstRound = 0, SecondRound = 0;
        bool Failed = false;
        TUniquePtr<AkUGCSceneVisual::FCapture> Visual;
        TArray<TSharedPtr<FJsonValue>> Checkpoints;
    };
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAkUGCRoomKeyboardTest, "AkUGC.Runtime.Room.OSKeyboardTwoRounds",
    EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FAkUGCRoomKeyboardTest::RunTest(const FString& Parameters)
{
    FString Output;
    if (!FParse::Value(FCommandLine::Get(), TEXT("RoomKeyboardOutput="), Output))
    { AddError(TEXT("Room keyboard test requires explicit OS driver; no keyboard flow was executed.")); return false; }
    if (FParse::Param(FCommandLine::Get(), TEXT("nullrhi"))
        || !FPaths::IsUnderDirectory(FPaths::ConvertRelativePathToFull(Output), FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir())))
    { AddError(TEXT("Room keyboard verification requires rendering and isolated Saved output.")); return false; }
    ADD_LATENT_AUTOMATION_COMMAND(AkUGCRoomKeyboard::FObserve(this, Output));
    return true;
}
#endif

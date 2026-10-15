#include "Misc/AutomationTest.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Game/AkUGCGameMode.h"
#include "Game/AkUGCGameState.h"
#include "Game/AkUGCPlayerController.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Pack/AkUGCWorkLibrary.h"
#include "Pack/AkUGCLogicPackHasher.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AkUGCWorkPlay
{
    class FObserve final : public IAutomationLatentCommand
    {
    public:
        FObserve(FAutomationTestBase* InTest) : Test(InTest), Start(FPlatformTime::Seconds()) {}
        bool Update() override
        {
            if (FPlatformTime::Seconds() - Start > 140) { Test->AddError(TEXT("Shared work play timed out.")); return true; }
            UWorld* World = nullptr;
            for (const auto& Context : GEngine->GetWorldContexts())
            { if (Context.World() && Context.World()->IsGameWorld() && Context.World()->GetNetMode() == NM_Standalone) { World = Context.World(); break; } }
            if (!World) { return false; }
            auto* Mode = World->GetAuthGameMode<AAkUGCGameMode>();
            auto* State = World->GetGameState<AAkUGCGameState>();
            auto* Player = Cast<AAkUGCPlayerController>(World->GetFirstPlayerController());
            if (!Mode || !State || !Player || !Mode->HasPreparedProject()) { return false; }
            FString IdText, Share, Key, BadShare, RegistryBase, RegistryToken;
            FString RegistryBadToken, RegistryBadKey;
            FParse::Value(FCommandLine::Get(), TEXT("WorkId="), IdText);
            FParse::Value(FCommandLine::Get(), TEXT("WorkShare="), Share);
            FParse::Value(FCommandLine::Get(), TEXT("WorkTrustedKey="), Key);
            FParse::Value(FCommandLine::Get(), TEXT("WorkBadShare="), BadShare);
            FParse::Value(FCommandLine::Get(), TEXT("RegistryBase="), RegistryBase);
            FParse::Value(FCommandLine::Get(), TEXT("RegistryToken="), RegistryToken);
            FParse::Value(FCommandLine::Get(), TEXT("RegistryBadToken="), RegistryBadToken);
            FParse::Value(FCommandLine::Get(), TEXT("RegistryBadTrustedKey="), RegistryBadKey);
            if (RegistryToken.IsEmpty())
            {
                RegistryToken = FPlatformMisc::GetEnvironmentVariable(
                    TEXT("AKUGC_PLAYER_REGISTRY_TOKEN"));
            }
            if (RegistryBadToken.IsEmpty())
            {
                RegistryBadToken = FPlatformMisc::GetEnvironmentVariable(
                    TEXT("AKUGC_PLAYER_REGISTRY_BAD_TOKEN"));
            }
            FGuid Id;
            if (!FGuid::ParseExact(IdText, EGuidFormats::Digits, Id)) { Test->AddError(TEXT("Invalid test work ID.")); return true; }
            FAkUGCWorkLibrary Library(Player->WorkLibraryDirectory());
            if (!bStarted)
            {
                FString Error;
                FAkUGCWorkEntry Entry;
                if (AcquisitionStage == 0)
                {
                    if (!RegistryBase.IsEmpty())
                    {
                        if (Library.Load(Id).bSucceeded)
                        {
                            Test->AddError(TEXT("Registry receiver must start with an independent empty library."));
                            return true;
                        }
                        if (FParse::Param(FCommandLine::Get(), TEXT("RegistryExerciseRejections")))
                        {
                            if (RegistryBadToken.IsEmpty() || RegistryBadKey.IsEmpty())
                            {
                                Test->AddError(TEXT("Registry rejection exercise arguments are missing."));
                                return true;
                            }
                            Player->FetchRegistryWork(RegistryBase, RegistryBadToken, IdText, Key);
                            AcquisitionStage = 1;
                        }
                        else
                        {
                            Player->FetchRegistryWork(RegistryBase, RegistryToken, IdText, Key);
                            AcquisitionStage = 3;
                        }
                        if (!Player->HasPendingRegistryWork())
                        {
                            Test->AddError(TEXT("Player did not start Registry acquisition."));
                            return true;
                        }
                        return false;
                    }
                    if (!Share.IsEmpty())
                    {
                        if (Library.Load(Id).bSucceeded) { Test->AddError(TEXT("Receiver must start with an independent empty library.")); return true; }
                        Player->ImportWork(Share, TEXT(""));
                        if (Library.Load(Id).bSucceeded) { Test->AddError(TEXT("Untrusted share was accepted.")); return true; }
                        Player->ImportWork(BadShare, Key);
                        if (Library.Load(Id).bSucceeded) { Test->AddError(TEXT("Tampered share was accepted.")); return true; }
                        Player->ImportWork(Share, Key);
                    }
                    AcquisitionStage = 4;
                }
                if (AcquisitionStage == 1)
                {
                    if (Player->HasPendingRegistryWork()) { return false; }
                    if (Player->GetRegistryFailureCode() != TEXT("unauthorized")
                        || Library.Load(Id).bSucceeded
                        || Mode->HasAuthoritySession())
                    {
                        Test->AddError(TEXT("Wrong Player token was not rejected without side effects."));
                        return true;
                    }
                    Player->FetchRegistryWork(RegistryBase, RegistryToken, IdText, RegistryBadKey);
                    AcquisitionStage = 2;
                    if (!Player->HasPendingRegistryWork())
                    {
                        Test->AddError(TEXT("Wrong-key Registry acquisition did not start."));
                        return true;
                    }
                    return false;
                }
                if (AcquisitionStage == 2)
                {
                    if (Player->HasPendingRegistryWork()) { return false; }
                    if (Player->GetRegistryFailureCode() != TEXT("local_verification")
                        || Library.Load(Id).bSucceeded
                        || Mode->HasAuthoritySession())
                    {
                        Test->AddError(TEXT("Wrong independent key was not rejected without side effects."));
                        return true;
                    }
                    Player->FetchRegistryWork(RegistryBase, RegistryToken, IdText, Key);
                    AcquisitionStage = 3;
                    if (!Player->HasPendingRegistryWork())
                    {
                        Test->AddError(TEXT("Valid Registry acquisition did not start."));
                        return true;
                    }
                    return false;
                }
                if (AcquisitionStage == 3)
                {
                    if (Player->HasPendingRegistryWork()) { return false; }
                    if (!Player->GetRegistryFailureCode().IsEmpty())
                    {
                        Test->AddError(TEXT("Valid Registry acquisition failed: ") + Player->GetWorkLibraryStatus());
                        return true;
                    }
                    AcquisitionStage = 4;
                }
                const auto Loaded = Library.Load(Id);
                if (!Loaded.bSucceeded) { Test->AddError(Loaded.ErrorMessage); return true; }
                ExpectedHash = FAkUGCLogicPackHasher::HashDocument(Loaded.Pack.Document);
                Player->ListWorks();
                Player->PlayWork(IdText);
                if (FAkUGCLogicPackHasher::HashDocument(Mode->GetPreparedDocument()) != ExpectedHash)
                { Test->AddError(TEXT("Player did not prepare exact received work.")); return true; }
                Player->StartSolo();
                if (!Mode->HasAuthoritySession()) { Test->AddError(TEXT("Shared work did not start.")); return true; }
                if (Mode->PrepareLibraryWork(Id, Player->WorkLibraryDirectory(), &Error))
                { Test->AddError(TEXT("Playing session allowed replacement.")); return true; }
                bStarted = true;
                UE_LOG(LogTemp, Display, TEXT("[AkUGCWorkPlay] Started Work=%s Source=%s Rejections=%d"),
                    *IdText,
                    RegistryBase.IsEmpty() ? (Share.IsEmpty() ? TEXT("Library") : TEXT("Share")) : TEXT("Registry"),
                    FParse::Param(FCommandLine::Get(), TEXT("RegistryExerciseRejections")) ? 2 : 0);
            }
            if (!State->GetScoreSnapshot().bSettled) { return false; }
            if (State->GetMatchResult() != EAkUGCTowerDefenseMatchResult::Victory || State->GetScoreSnapshot().TeamScore <= 0)
            { Test->AddError(TEXT("Shared work did not complete victory with score.")); return true; }
            FString Capture;
            if (FParse::Value(FCommandLine::Get(), TEXT("AkUGCHUDCapture="), Capture) && IFileManager::Get().FileSize(*Capture) <= 0) { return false; }
            const int32 Score = State->GetScoreSnapshot().TeamScore;
            Player->ReturnSolo();
            if (Mode->HasAuthoritySession() || State->GetMatchFlow() != EAkUGCMatchFlow::Preparation
                || FAkUGCLogicPackHasher::HashDocument(Mode->GetPreparedDocument()) != ExpectedHash)
            { Test->AddError(TEXT("Shared work replay preparation changed authored content.")); return true; }
            UE_LOG(LogTemp, Display, TEXT("[AkUGCWorkPlay] Passed Work=%s Score=%d Source=%s Returned=1"),
                *IdText, Score,
                RegistryBase.IsEmpty() ? (Share.IsEmpty() ? TEXT("Library") : TEXT("Share")) : TEXT("Registry"));
            return true;
        }
    private:
        FAutomationTestBase* Test;
        double Start;
        FString ExpectedHash;
        bool bStarted = false;
        int32 AcquisitionStage = 0;
    };

    class FObserveRegistryRejection final : public IAutomationLatentCommand
    {
    public:
        FObserveRegistryRejection(FAutomationTestBase* InTest)
            : Test(InTest), Start(FPlatformTime::Seconds()) {}

        bool Update() override
        {
            if (FPlatformTime::Seconds() - Start > 75)
            {
                Test->AddError(TEXT("Registry rejection test timed out."));
                return true;
            }
            UWorld* World = nullptr;
            for (const auto& Context : GEngine->GetWorldContexts())
            {
                if (Context.World() && Context.World()->IsGameWorld()
                    && Context.World()->GetNetMode() == NM_Standalone)
                {
                    World = Context.World();
                    break;
                }
            }
            if (!World) { return false; }
            auto* Mode = World->GetAuthGameMode<AAkUGCGameMode>();
            auto* Player = Cast<AAkUGCPlayerController>(
                World->GetFirstPlayerController());
            if (!Mode || !Player || !Mode->HasPreparedProject())
            {
                return false;
            }

            FString IdText, Base, Token, Key, ExpectedFailure;
            FParse::Value(FCommandLine::Get(), TEXT("WorkId="), IdText);
            FParse::Value(FCommandLine::Get(), TEXT("RegistryBase="), Base);
            FParse::Value(FCommandLine::Get(), TEXT("RegistryToken="), Token);
            FParse::Value(FCommandLine::Get(), TEXT("WorkTrustedKey="), Key);
            FParse::Value(
                FCommandLine::Get(),
                TEXT("RegistryExpectedFailure="),
                ExpectedFailure);
            if (Token.IsEmpty())
            {
                Token = FPlatformMisc::GetEnvironmentVariable(
                    TEXT("AKUGC_PLAYER_REGISTRY_TOKEN"));
            }
            FGuid Id;
            if (!FGuid::ParseExact(IdText, EGuidFormats::Digits, Id)
                || Base.IsEmpty() || Token.IsEmpty() || Key.IsEmpty()
                || ExpectedFailure.IsEmpty())
            {
                Test->AddError(TEXT("Registry rejection arguments are invalid."));
                return true;
            }
            FAkUGCWorkLibrary Library(Player->WorkLibraryDirectory());
            if (!bRequested)
            {
                if (Library.Load(Id).bSucceeded)
                {
                    Test->AddError(TEXT("Rejected download test requires an empty library."));
                    return true;
                }
                InitialHash =
                    FAkUGCLogicPackHasher::HashDocument(
                        Mode->GetPreparedDocument());
                Player->FetchRegistryWork(Base, Token, IdText, Key);
                if (!Player->HasPendingRegistryWork())
                {
                    Test->AddError(
                        TEXT("Rejected Registry download did not start: ")
                        + Player->GetWorkLibraryStatus());
                    return true;
                }
                bRequested = true;
                return false;
            }
            if (Player->HasPendingRegistryWork()) { return false; }
            if (Player->GetRegistryFailureCode() != ExpectedFailure)
            {
                Test->AddError(FString::Printf(
                    TEXT("Registry rejection mismatch: expected %s, got %s (%s)"),
                    *ExpectedFailure,
                    *Player->GetRegistryFailureCode(),
                    *Player->GetWorkLibraryStatus()));
                return true;
            }
            if (Library.Load(Id).bSucceeded
                || Mode->HasAuthoritySession()
                || FAkUGCLogicPackHasher::HashDocument(
                    Mode->GetPreparedDocument()) != InitialHash)
            {
                Test->AddError(TEXT("Rejected Registry download changed library or session state."));
                return true;
            }
            UE_LOG(LogTemp, Display,
                TEXT("[AkUGCPlayerRegistryReject] Passed Work=%s Code=%s LibraryEmpty=1 SessionStarted=0"),
                *IdText,
                *ExpectedFailure);
            return true;
        }

    private:
        FAutomationTestBase* Test;
        double Start;
        FString InitialHash;
        bool bRequested = false;
    };
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAkUGCWorkPlayTest, "AkUGC.Runtime.WorkLibrary.SharedPlayable",
    EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FAkUGCWorkPlayTest::RunTest(const FString& Parameters)
{
    if (!FParse::Param(FCommandLine::Get(), TEXT("AkUGCWorkPlay"))) { AddWarning(TEXT("Requires -AkUGCWorkPlay with published work and independent library.")); return true; }
    ADD_LATENT_AUTOMATION_COMMAND(AkUGCWorkPlay::FObserve(this));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCPlayerRegistryRejectedDownloadTest,
    "AkUGC.Runtime.ReleaseRegistry.DownloadRejected",
    EAutomationTestFlags::ClientContext
        | EAutomationTestFlags::EngineFilter)
bool FAkUGCPlayerRegistryRejectedDownloadTest::RunTest(
    const FString& Parameters)
{
    if (!FParse::Param(
        FCommandLine::Get(),
        TEXT("AkUGCRegistryReject")))
    {
        AddWarning(TEXT("Requires -AkUGCRegistryReject with Registry fixture arguments."));
        return true;
    }
    ADD_LATENT_AUTOMATION_COMMAND(
        AkUGCWorkPlay::FObserveRegistryRejection(this));
    return true;
}
#endif

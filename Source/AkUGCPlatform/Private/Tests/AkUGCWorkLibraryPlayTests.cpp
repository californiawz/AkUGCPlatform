#include "Misc/AutomationTest.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Game/AkUGCGameMode.h"
#include "Game/AkUGCGameState.h"
#include "Game/AkUGCPlayerController.h"
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
            FString IdText, Share, Key, BadShare;
            FParse::Value(FCommandLine::Get(), TEXT("WorkId="), IdText);
            FParse::Value(FCommandLine::Get(), TEXT("WorkShare="), Share);
            FParse::Value(FCommandLine::Get(), TEXT("WorkTrustedKey="), Key);
            FParse::Value(FCommandLine::Get(), TEXT("WorkBadShare="), BadShare);
            FGuid Id;
            if (!FGuid::ParseExact(IdText, EGuidFormats::Digits, Id)) { Test->AddError(TEXT("Invalid test work ID.")); return true; }
            FAkUGCWorkLibrary Library(Player->WorkLibraryDirectory());
            if (!bStarted)
            {
                FString Error;
                FAkUGCWorkEntry Entry;
                if (!Share.IsEmpty())
                {
                    if (Library.Load(Id).bSucceeded) { Test->AddError(TEXT("Receiver must start with an independent empty library.")); return true; }
                    Player->ImportWork(Share, TEXT(""));
                    if (Library.Load(Id).bSucceeded) { Test->AddError(TEXT("Untrusted share was accepted.")); return true; }
                    Player->ImportWork(BadShare, Key);
                    if (Library.Load(Id).bSucceeded) { Test->AddError(TEXT("Tampered share was accepted.")); return true; }
                    Player->ImportWork(Share, Key);
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
                UE_LOG(LogTemp, Display, TEXT("[AkUGCWorkPlay] Started Work=%s Restart=%d"), *IdText, Share.IsEmpty());
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
            UE_LOG(LogTemp, Display, TEXT("[AkUGCWorkPlay] Passed Work=%s Score=%d Restart=%d Returned=1"), *IdText, Score, Share.IsEmpty());
            return true;
        }
    private:
        FAutomationTestBase* Test;
        double Start;
        FString ExpectedHash;
        bool bStarted = false;
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
#endif

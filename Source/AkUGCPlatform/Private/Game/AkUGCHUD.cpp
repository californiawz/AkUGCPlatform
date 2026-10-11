#include "Game/AkUGCHUD.h"

#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/GameViewportClient.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

FString AAkUGCHUD::FormatSettlement(const FAkUGCTowerDefenseScoreSnapshot& Score)
{
    if (!Score.bSettled) { return TEXT("Awaiting authoritative settlement"); }
    const double HealthPercent = Score.FinalBaseMaximumHealth > 0.0
        ? 100.0 * Score.FinalBaseCurrentHealth / Score.FinalBaseMaximumHealth : 0.0;
    return FString::Printf(TEXT("%s\nTEAM SCORE  %d\nENEMIES DEFEATED  %d\nBASE PRESERVED  %.1f%%\nRESOURCES  %d"),
        Score.Result == EAkUGCTowerDefenseMatchResult::Victory ? TEXT("VICTORY") : TEXT("DEFEAT"),
        Score.TeamScore, Score.EnemyKills, HealthPercent, Score.Resources);
}

void AAkUGCHUD::DrawHUD()
{
    Super::DrawHUD();
    if (!Canvas || !GetWorld() || GetNetMode() == NM_DedicatedServer) { return; }
    const AAkUGCGameState* State = GetWorld()->GetGameState<AAkUGCGameState>();
    if (!State) { return; }
    const FAkUGCWaveRuntimeSnapshot Wave = State->GetWaveSnapshot();
    const FAkUGCTowerDefenseScoreSnapshot Score = State->GetScoreSnapshot();
    const float Width = FMath::Min(360.0f, Canvas->ClipX - 32.0f);
    if (Width < 120.0f) { return; }
    UFont* Font = GEngine ? GEngine->GetSmallFont() : nullptr;
    DrawRect(FLinearColor(0.025f, 0.045f, 0.07f, 0.92f), 16.0f, 16.0f, Width, 174.0f);
    DrawRect(FLinearColor(0.0f, 0.7f, 0.65f), 16.0f, 16.0f, 4.0f, 174.0f);
    DrawText(TEXT("AKUGC  /  CO-OP TOWER DEFENSE"), FLinearColor(0.3f, 0.95f, 0.85f), 30.0f, 27.0f, Font, 1.15f);
    DrawText(FString::Printf(TEXT("WAVE %d / %d   |   ACTIVE ENEMIES %d"),
        Wave.CurrentWaveIndex >= 0 ? Wave.CurrentWaveIndex + 1 : 0, Wave.TotalWaveCount, State->GetActiveEnemyCount()),
        FLinearColor::White, 30.0f, 57.0f, Font);
    DrawText(FString::Printf(TEXT("BASE  %.0f / %.0f"), State->GetBaseHealthCurrent(), State->GetBaseHealthMaximum()),
        FLinearColor::White, 30.0f, 83.0f, Font);
    const float HealthRatio = State->GetBaseHealthMaximum() > 0.0
        ? FMath::Clamp(static_cast<float>(State->GetBaseHealthCurrent() / State->GetBaseHealthMaximum()), 0.0f, 1.0f) : 0.0f;
    DrawRect(FLinearColor(0.15f, 0.2f, 0.25f), 30.0f, 106.0f, Width - 28.0f, 7.0f);
    DrawRect(FLinearColor(0.0f, 0.75f, 0.6f), 30.0f, 106.0f, (Width - 28.0f) * HealthRatio, 7.0f);
    DrawText(FString::Printf(TEXT("RESOURCES %d    TEAM SCORE %d"), Score.Resources, Score.TeamScore),
        FLinearColor(1.0f, 0.82f, 0.35f), 30.0f, 126.0f, Font);
    DrawText(TEXT("Individual score: unassigned shared map towers"), FLinearColor(0.55f, 0.65f, 0.75f), 30.0f, 155.0f, Font, 0.9f);
    if (Score.bSettled)
    {
        const float PanelWidth = FMath::Min(420.0f, Canvas->ClipX - 32.0f);
        const float X = (Canvas->ClipX - PanelWidth) * 0.5f;
        const float Y = FMath::Max(200.0f, (Canvas->ClipY - 190.0f) * 0.5f);
        DrawRect(FLinearColor(0.025f, 0.045f, 0.07f, 0.96f), X, Y, PanelWidth, 190.0f);
        DrawText(FormatSettlement(Score), Score.Result == EAkUGCTowerDefenseMatchResult::Victory
            ? FLinearColor(0.3f, 0.95f, 0.7f) : FLinearColor(1.0f, 0.4f, 0.35f), X + 24.0f, Y + 22.0f, Font, 1.5f);
    }
    else if (Wave.Result != EAkUGCTowerDefenseMatchResult::InProgress)
    {
        DrawText(TEXT("Waiting for authoritative score..."), FLinearColor::White, 30.0f, 200.0f, Font);
    }
#if WITH_DEV_AUTOMATION_TESTS
    FString CapturePath;
    if (Score.bSettled && !bRequestedCapture && FParse::Value(FCommandLine::Get(), TEXT("AkUGCHUDCapture="), CapturePath))
    {
        FScreenshotRequest::RequestScreenshot(CapturePath, false, false);
        bRequestedCapture = true;
    }
#endif
}

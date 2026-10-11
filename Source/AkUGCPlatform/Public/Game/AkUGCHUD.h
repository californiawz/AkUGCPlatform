#pragma once

#include "GameFramework/HUD.h"
#include "Game/AkUGCGameState.h"
#include "AkUGCHUD.generated.h"

UCLASS()
class AKUGCPLATFORM_API AAkUGCHUD : public AHUD
{
    GENERATED_BODY()

public:
    virtual void DrawHUD() override;
    static FString FormatSettlement(const FAkUGCTowerDefenseScoreSnapshot& Score);

private:
    bool bRequestedCapture = false;
};

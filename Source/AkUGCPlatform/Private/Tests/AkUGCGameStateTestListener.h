#pragma once

#include "CoreMinimal.h"
#include "Subsystem/AkUGCLogicRuntimeSubsystem.h"
#include "AkUGCGameStateTestListener.generated.h"

UCLASS()
class UAkUGCMatchEndedTestListener : public UObject
{
    GENERATED_BODY()

public:
    int32 BroadcastCount = 0;
    EAkUGCTowerDefenseMatchResult LastResult = EAkUGCTowerDefenseMatchResult::InProgress;

    UFUNCTION()
    void OnMatchEnded(EAkUGCTowerDefenseMatchResult Result)
    {
        ++BroadcastCount;
        LastResult = Result;
    }
};

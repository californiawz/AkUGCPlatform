#pragma once

#include "GameFramework/PlayerController.h"
#include "AkUGCPlayerController.generated.h"

UCLASS()
class AKUGCPLATFORM_API AAkUGCPlayerController : public APlayerController
{
    GENERATED_BODY()

public:
    virtual void BeginPlay() override;
    virtual void SetupInputComponent() override;

    UFUNCTION(Exec)
    void StartSolo();

    UFUNCTION(Exec)
    void ReturnSolo();
};

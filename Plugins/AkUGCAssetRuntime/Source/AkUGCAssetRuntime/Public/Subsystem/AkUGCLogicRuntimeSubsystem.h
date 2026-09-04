#pragma once

#include "CoreMinimal.h"
#include "Document/AkUGCDocument.h"
#include "Subsystems/WorldSubsystem.h"
#include "AkUGCLogicRuntimeSubsystem.generated.h"

USTRUCT(BlueprintType)
struct AKUGCASSETRUNTIME_API FAkUGCLogicRuntimeMessage
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Logic")
    FGuid SourceNodeId;

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Logic")
    FString Message;
};

USTRUCT(BlueprintType)
struct AKUGCASSETRUNTIME_API FAkUGCLogicRuntimeResult
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Logic")
    bool bSucceeded = false;

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Logic")
    FString ErrorPath;

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Logic")
    FString ErrorMessage;

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Logic")
    int32 ExecutedInstructionCount = 0;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
    FAkUGCLogicMessageDelegate,
    FGuid,
    SourceNodeId,
    const FString&,
    Message);

UCLASS()
class AKUGCASSETRUNTIME_API UAkUGCLogicRuntimeSubsystem : public UWorldSubsystem
{
    GENERATED_BODY()

public:
    UPROPERTY(BlueprintAssignable, Category = "UGC|Logic")
    FAkUGCLogicMessageDelegate OnMessage;

    UFUNCTION(BlueprintCallable, Category = "UGC|Logic")
    FAkUGCLogicRuntimeResult RunGameStart(const FAkUGCLogicGraph& LogicGraph);

    UFUNCTION(BlueprintCallable, Category = "UGC|Logic")
    void ResetLogicRuntime();

    UFUNCTION(BlueprintPure, Category = "UGC|Logic")
    TArray<FAkUGCLogicRuntimeMessage> GetEmittedMessages() const;

protected:
    virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
    TArray<FAkUGCLogicRuntimeMessage> EmittedMessages;
    bool bIsRunning = false;
};

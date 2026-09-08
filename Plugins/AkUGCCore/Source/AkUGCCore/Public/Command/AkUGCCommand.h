#pragma once

#include "CoreMinimal.h"
#include "Document/AkUGCDocument.h"
#include "AkUGCCommand.generated.h"

UENUM(BlueprintType)
enum class EAkUGCCommandType : uint8
{
    AddEntity,
    DeleteEntity,
    SetTransform,
    SetProperty,
    RemoveProperty,
    DuplicateEntity,
    SetParent,
    AddLogicNode,
    DeleteLogicNode,
    ConnectLogicNode,
    DisconnectLogicNode,
    AddWave,
    UpdateWave,
    DeleteWave,
    MoveWave,
    SetRulesetSettings,
    UpdateLogicNode
};

USTRUCT(BlueprintType)
struct AKUGCCORE_API FAkUGCCommand
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    FGuid CommandId;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    FGuid AuthorId;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    int64 Sequence = 0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    EAkUGCCommandType Type = EAkUGCCommandType::AddEntity;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    FGuid SceneId;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    FGuid EntityId;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    FGuid SourceEntityId;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    FGuid ParentEntityId;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    FAkUGCEntityRecord Entity;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    FTransform Transform = FTransform::Identity;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    FName ComponentTypeId;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    FName PropertyId;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    FAkUGCValue PropertyValue;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC|Logic")
    FAkUGCLogicNode LogicNode;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC|Logic")
    FAkUGCLogicConnection LogicConnection;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC|Logic")
    TArray<FAkUGCLogicConnection> LogicConnections;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC|Ruleset")
    FAkUGCTowerDefenseWave Wave;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC|Ruleset")
    int32 WaveIndex = INDEX_NONE;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC|Ruleset")
    double WaveIntervalSeconds = 5.0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC|Ruleset")
    EAkUGCTowerDefenseDefeatCondition DefeatCondition = EAkUGCTowerDefenseDefeatCondition::BaseHealthDepleted;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC|Ruleset")
    EAkUGCTowerDefenseVictoryCondition VictoryCondition = EAkUGCTowerDefenseVictoryCondition::AllWavesCleared;
};

USTRUCT(BlueprintType)
struct AKUGCCORE_API FAkUGCCommandTransaction
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    FGuid TransactionId;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    FString Label;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    TArray<FAkUGCCommand> Commands;
};

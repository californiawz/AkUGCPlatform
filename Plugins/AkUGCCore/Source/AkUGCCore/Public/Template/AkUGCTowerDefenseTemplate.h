#pragma once

#include "CoreMinimal.h"
#include "Command/AkUGCCommand.h"
#include "Document/AkUGCDocument.h"
#include "AkUGCTowerDefenseTemplate.generated.h"

class FAkUGCPrefabRegistry;

USTRUCT(BlueprintType)
struct AKUGCCORE_API FAkUGCTowerDefenseWaveParameters
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC|Template")
    FName EnemyPrefabId = TEXT("official.unit.basic_enemy");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC|Template")
    int32 EnemyCount = 10;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC|Template")
    double SpawnIntervalSeconds = 1.0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC|Template")
    double EnemyMaxHealth = 100.0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC|Template")
    double EnemyMoveSpeed = 300.0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC|Template")
    double EnemyGoalDamage = 10.0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC|Template")
    double StartDelaySeconds = 0.0;
};

USTRUCT(BlueprintType)
struct AKUGCCORE_API FAkUGCTowerDefenseParameters
{
    GENERATED_BODY()

    FAkUGCTowerDefenseParameters() { Waves.SetNum(AkUGCTowerDefenseRulesetLimits::RequiredWaveCount); }

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC|Template")
    int32 SchemaVersion = 1;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC|Template")
    TArray<FAkUGCTowerDefenseWaveParameters> Waves;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC|Template")
    int32 InitialResources = 200;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC|Template")
    double BaseMaxHealth = 1000.0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC|Template")
    double WaveIntervalSeconds = 5.0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC|Template")
    EAkUGCTowerDefenseDefeatCondition DefeatCondition = EAkUGCTowerDefenseDefeatCondition::BaseHealthDepleted;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC|Template")
    EAkUGCTowerDefenseVictoryCondition VictoryCondition = EAkUGCTowerDefenseVictoryCondition::AllWavesCleared;
};

class AKUGCCORE_API FAkUGCTowerDefenseTemplate
{
public:
    static bool CreateDefaultDocument(const FAkUGCPrefabRegistry& Registry,
        FAkUGCProjectDocument& OutDocument, FString* OutError = nullptr);
    static bool ReadParameters(const FAkUGCProjectDocument& Document, const FGuid& SceneId,
        FAkUGCTowerDefenseParameters& OutParameters, FString* OutError = nullptr);
    static bool BuildTransaction(const FAkUGCProjectDocument& Document, const FGuid& SceneId,
        const FAkUGCTowerDefenseParameters& Parameters, const FAkUGCPrefabRegistry& Registry,
        FAkUGCCommandTransaction& OutTransaction, FString* OutError = nullptr);
    static bool Validate(const FAkUGCTowerDefenseParameters& Parameters,
        const FAkUGCPrefabRegistry& Registry, FString* OutError = nullptr);
    static bool ValidateDocumentBudgets(const FAkUGCProjectDocument& Document,
        const FAkUGCPrefabRegistry& Registry, FString* OutError = nullptr);
    static bool IsTemplateOwnedLogicGraph(const FAkUGCSceneDocument& Scene);
    static bool SerializeParameters(const FAkUGCTowerDefenseParameters& Parameters,
        FString& OutJson, FString* OutError = nullptr);
    static bool DeserializeParameters(const FString& Json,
        FAkUGCTowerDefenseParameters& OutParameters, FString* OutError = nullptr);
};

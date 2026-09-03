#pragma once

#include "CoreMinimal.h"
#include "Document/AkUGCDocument.h"
#include "AkUGCPrefabDefinition.generated.h"

UENUM(BlueprintType, meta = (Bitflags))
enum class EAkUGCTargetPlatform : uint8
{
    None = 0,
    Win64 = 1 << 0,
    Android = 1 << 1,
    IOS = 1 << 2,
    Server = 1 << 3
};
ENUM_CLASS_FLAGS(EAkUGCTargetPlatform)

USTRUCT(BlueprintType)
struct AKUGCCORE_API FAkUGCPropertyDefinition
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    FName PropertyId;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    EAkUGCValueType ValueType = EAkUGCValueType::String;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    FAkUGCValue DefaultValue;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    bool bRequired = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    bool bMobileEditable = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    bool bHasMinimum = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC", meta = (EditCondition = "bHasMinimum"))
    double Minimum = 0.0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    bool bHasMaximum = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC", meta = (EditCondition = "bHasMaximum"))
    double Maximum = 0.0;
};

USTRUCT(BlueprintType)
struct AKUGCCORE_API FAkUGCPrefabCost
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC", meta = (ClampMin = "0"))
    int32 EntityUnits = 1;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC", meta = (ClampMin = "0"))
    int32 RenderUnits = 1;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC", meta = (ClampMin = "0"))
    int32 PhysicsUnits = 0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC", meta = (ClampMin = "0"))
    int32 ScriptUnits = 0;
};

USTRUCT(BlueprintType)
struct AKUGCCORE_API FAkUGCPlacementRules
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    bool bSnapToGround = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    bool bAllowScale = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    FVector MinimumScale = FVector(1.0);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    FVector MaximumScale = FVector(1.0);
};

USTRUCT(BlueprintType)
struct AKUGCCORE_API FAkUGCPrefabDefinition
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    FName PrefabId;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC", meta = (ClampMin = "1"))
    int32 DefinitionVersion = 1;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    FString DisplayName;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    FName EntityType;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC", meta = (Bitmask, BitmaskEnum = "/Script/AkUGCCore.EAkUGCTargetPlatform"))
    int32 SupportedPlatforms = static_cast<int32>(EAkUGCTargetPlatform::Win64)
        | static_cast<int32>(EAkUGCTargetPlatform::Android)
        | static_cast<int32>(EAkUGCTargetPlatform::Server);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    FSoftObjectPath Thumbnail;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    TMap<FName, FSoftObjectPath> AssetVariants;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    TArray<FAkUGCComponentRecord> DefaultComponents;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    TArray<FAkUGCPropertyDefinition> EditableProperties;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    FAkUGCPlacementRules Placement;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    FAkUGCPrefabCost Cost;
};

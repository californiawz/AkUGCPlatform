#pragma once

#include "CoreMinimal.h"
#include "AkUGCDocument.generated.h"

namespace AkUGCSchema
{
    inline constexpr int32 OldestSupportedProjectDocumentVersion = 0;
    inline constexpr int32 CurrentProjectDocumentVersion = 1;
}

UENUM(BlueprintType)
enum class EAkUGCValueType : uint8
{
    Bool,
    Integer,
    Number,
    String,
    Name,
    Vector,
    Rotator
};

USTRUCT(BlueprintType)
struct AKUGCCORE_API FAkUGCValue
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    EAkUGCValueType Type = EAkUGCValueType::String;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    bool BoolValue = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    int64 IntegerValue = 0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    double NumberValue = 0.0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    FString StringValue;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    FName NameValue;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    FVector VectorValue = FVector::ZeroVector;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    FRotator RotatorValue = FRotator::ZeroRotator;
};

USTRUCT(BlueprintType)
struct AKUGCCORE_API FAkUGCComponentRecord
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    FName TypeId;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC", meta = (ClampMin = "1"))
    int32 SchemaVersion = 1;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    TMap<FName, FAkUGCValue> Properties;
};

USTRUCT(BlueprintType)
struct AKUGCCORE_API FAkUGCEntityRecord
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    FGuid EntityId;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    FName PrefabId;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    FTransform Transform = FTransform::Identity;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    FGuid ParentEntityId;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    TArray<FName> Tags;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    TArray<FAkUGCComponentRecord> Components;
};

UENUM(BlueprintType)
enum class EAkUGCLogicNodeType : uint8
{
    GameStart,
    Message
};

USTRUCT(BlueprintType)
struct AKUGCCORE_API FAkUGCLogicNode
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC|Logic")
    FGuid NodeId;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC|Logic")
    EAkUGCLogicNodeType Type = EAkUGCLogicNodeType::GameStart;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC|Logic")
    FString Message;
};

USTRUCT(BlueprintType)
struct AKUGCCORE_API FAkUGCLogicConnection
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC|Logic")
    FGuid SourceNodeId;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC|Logic")
    FGuid TargetNodeId;
};

USTRUCT(BlueprintType)
struct AKUGCCORE_API FAkUGCLogicGraph
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC|Logic")
    TArray<FAkUGCLogicNode> Nodes;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC|Logic")
    TArray<FAkUGCLogicConnection> Connections;
};

USTRUCT(BlueprintType)
struct AKUGCCORE_API FAkUGCSceneDocument
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    FGuid SceneId;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    FString DisplayName;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    TArray<FAkUGCEntityRecord> Entities;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC|Logic")
    FAkUGCLogicGraph LogicGraph;
};

USTRUCT(BlueprintType)
struct AKUGCCORE_API FAkUGCProjectManifest
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    int32 SchemaVersion = AkUGCSchema::CurrentProjectDocumentVersion;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    FGuid ProjectId;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    FString DisplayName;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    FName TemplateId;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    TArray<FName> Capabilities;
};

USTRUCT(BlueprintType)
struct AKUGCCORE_API FAkUGCProjectDocument
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    FAkUGCProjectManifest Manifest;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC")
    TArray<FAkUGCSceneDocument> Scenes;
};

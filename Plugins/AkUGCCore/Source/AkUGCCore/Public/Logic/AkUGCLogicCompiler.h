#pragma once

#include "CoreMinimal.h"
#include "Document/AkUGCDocument.h"
#include "AkUGCLogicCompiler.generated.h"

UENUM(BlueprintType)
enum class EAkUGCLogicOpcode : uint8
{
    GameStart,
    WaveStart,
    Message,
    Timer,
    Spawn
};

USTRUCT(BlueprintType)
struct AKUGCCORE_API FAkUGCLogicInstruction
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Logic")
    EAkUGCLogicOpcode Opcode = EAkUGCLogicOpcode::GameStart;

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Logic")
    FGuid SourceNodeId;

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Logic")
    FString Operand;

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Logic")
    double DelaySeconds = 0.0;

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Logic")
    FName SpawnPrefabId;

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Logic")
    FGuid SpawnAtEntityId;

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Logic")
    TArray<int32> SuccessorIndices;
};

USTRUCT(BlueprintType)
struct AKUGCCORE_API FAkUGCLogicProgram
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Logic")
    TArray<FAkUGCLogicInstruction> Instructions;

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Logic")
    int32 GameStartEntryIndex = INDEX_NONE;

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Logic")
    int32 WaveStartEntryIndex = INDEX_NONE;
};

struct AKUGCCORE_API FAkUGCLogicCompileResult
{
    bool bSucceeded = false;
    FString ErrorPath;
    FString ErrorMessage;
    FAkUGCLogicProgram Program;
};

class AKUGCCORE_API FAkUGCLogicCompiler
{
public:
    static FAkUGCLogicCompileResult Compile(const FAkUGCLogicGraph& LogicGraph);
};

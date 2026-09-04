#pragma once

#include "CoreMinimal.h"
#include "Document/AkUGCDocument.h"
#include "AkUGCLogicCompiler.generated.h"

UENUM(BlueprintType)
enum class EAkUGCLogicOpcode : uint8
{
    GameStart,
    Message
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
};

USTRUCT(BlueprintType)
struct AKUGCCORE_API FAkUGCLogicProgram
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "UGC|Logic")
    TArray<FAkUGCLogicInstruction> Instructions;
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

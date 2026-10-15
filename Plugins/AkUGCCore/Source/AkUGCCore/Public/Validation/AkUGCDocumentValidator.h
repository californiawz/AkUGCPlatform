#pragma once

#include "CoreMinimal.h"

struct FAkUGCLogicGraph;
struct FAkUGCProjectDocument;

enum class EAkUGCValidationSeverity : uint8
{
    Warning,
    Error
};

struct AKUGCCORE_API FAkUGCValidationIssue
{
    EAkUGCValidationSeverity Severity = EAkUGCValidationSeverity::Error;
    FString Path;
    FString Message;
};

struct AKUGCCORE_API FAkUGCValidationResult
{
    TArray<FAkUGCValidationIssue> Issues;

    bool IsValid() const;
    void AddError(FString Path, FString Message);
    void AddWarning(FString Path, FString Message);
};

class AKUGCCORE_API FAkUGCDocumentValidator
{
public:
    /** Resource-shape limits that every deserialized document must satisfy before use. */
    static FAkUGCValidationResult ValidateInputBudgets(
        const FAkUGCProjectDocument& Document);
    static FAkUGCValidationResult Validate(const FAkUGCProjectDocument& Document);
    static FAkUGCValidationResult ValidateLogicGraph(
        const FAkUGCLogicGraph& LogicGraph,
        const FString& Path = TEXT("logicGraph"));
};

#pragma once

#include "CoreMinimal.h"
#include "Template/AkUGCTowerDefenseTemplate.h"

struct AKUGCCORE_API FAkUGCCopilotPreview
{
    FString BaseDocumentHash;
    FAkUGCTowerDefenseParameters Parameters;
    FString Diff;
    FString ProposalJson;
    FString ProposalHash;
    FAkUGCCommandTransaction Transaction;
    bool bValidated = false;
};

class AKUGCCORE_API FAkUGCCopilotValidation
{
public:
    static bool Preview(const FAkUGCProjectDocument& Document, const FString& ParameterJson,
        FAkUGCCopilotPreview& OutPreview, FString* OutError = nullptr);
    static bool IsCurrent(const FAkUGCProjectDocument& Document, const FAkUGCCopilotPreview& Preview);
    static FString ExplainError(const FString& Error);
};

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

struct AKUGCCORE_API FAkUGCDocumentMigrationResult
{
    bool bSucceeded = false;
    int32 SourceVersion = INDEX_NONE;
    int32 TargetVersion = INDEX_NONE;
    TArray<FString> AppliedSteps;
    FString ErrorPath;
    FString ErrorMessage;
};

class AKUGCCORE_API FAkUGCDocumentMigrator
{
public:
    static FAkUGCDocumentMigrationResult Migrate(const TSharedRef<FJsonObject>& RootObject);
};

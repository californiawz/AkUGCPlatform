#pragma once

#include "CoreMinimal.h"

struct FAkUGCDocumentMigrationResult;
struct FAkUGCProjectDocument;

class AKUGCCORE_API FAkUGCDocumentJson
{
public:
    static bool ValidateNoDuplicateFields(const FString& Json, FString* OutError = nullptr);
    static bool Serialize(const FAkUGCProjectDocument& Document, FString& OutJson, FString* OutError = nullptr);
    static bool Deserialize(
        const FString& Json,
        FAkUGCProjectDocument& OutDocument,
        FString* OutError = nullptr,
        FAkUGCDocumentMigrationResult* OutMigration = nullptr);
};

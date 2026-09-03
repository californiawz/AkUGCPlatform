#pragma once

#include "CoreMinimal.h"

struct FAkUGCProjectDocument;

class AKUGCCORE_API FAkUGCDocumentJson
{
public:
    static bool Serialize(const FAkUGCProjectDocument& Document, FString& OutJson, FString* OutError = nullptr);
    static bool Deserialize(const FString& Json, FAkUGCProjectDocument& OutDocument, FString* OutError = nullptr);
};

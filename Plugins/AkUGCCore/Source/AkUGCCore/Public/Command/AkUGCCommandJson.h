#pragma once

#include "CoreMinimal.h"

struct FAkUGCCommandTransaction;

class AKUGCCORE_API FAkUGCCommandJson
{
public:
    static bool Serialize(const FAkUGCCommandTransaction& Transaction, FString& OutJson, FString* OutError = nullptr);
    static bool Deserialize(const FString& Json, FAkUGCCommandTransaction& OutTransaction, FString* OutError = nullptr);
};

#include "Command/AkUGCCommandJson.h"

#include "Command/AkUGCCommand.h"
#include "JsonObjectConverter.h"

bool FAkUGCCommandJson::Serialize(
    const FAkUGCCommandTransaction& Transaction,
    FString& OutJson,
    FString* OutError)
{
    OutJson.Reset();
    const bool bSucceeded = FJsonObjectConverter::UStructToJsonObjectString(Transaction, OutJson);
    if (!bSucceeded && OutError)
    {
        *OutError = TEXT("Failed to serialize UGC command transaction.");
    }
    return bSucceeded;
}

bool FAkUGCCommandJson::Deserialize(
    const FString& Json,
    FAkUGCCommandTransaction& OutTransaction,
    FString* OutError)
{
    OutTransaction = FAkUGCCommandTransaction{};
    const bool bSucceeded = FJsonObjectConverter::JsonObjectStringToUStruct(Json, &OutTransaction);
    if (!bSucceeded && OutError)
    {
        *OutError = TEXT("Failed to deserialize UGC command transaction.");
    }
    return bSucceeded;
}

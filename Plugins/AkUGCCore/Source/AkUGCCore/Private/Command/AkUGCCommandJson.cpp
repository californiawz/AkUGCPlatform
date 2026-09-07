#include "Command/AkUGCCommandJson.h"

#include "Command/AkUGCCommand.h"
#include "JsonObjectConverter.h"
#include "String/LexFromString.h"
#include "UObject/UnrealType.h"

namespace
{
    constexpr double MaxExactCommandJsonInteger = 9007199254740991.0;

    TSharedPtr<FJsonValue> ExportCommandProperty(FProperty* Property, const void* Value)
    {
        if (CastField<FInt64Property>(Property))
        {
            return MakeShared<FJsonValueString>(LexToString(*static_cast<const int64*>(Value)));
        }
        return nullptr;
    }

    bool ImportCommandProperty(
        const TSharedPtr<FJsonValue>& JsonValue,
        FProperty* Property,
        void* Value)
    {
        FInt64Property* Int64Property = CastField<FInt64Property>(Property);
        if (!Int64Property)
        {
            return false;
        }

        int64 ParsedValue = 0;
        if (JsonValue.IsValid() && JsonValue->Type == EJson::String)
        {
            if (!LexTryParseString(ParsedValue, *JsonValue->AsString()))
            {
                return false;
            }
        }
        else if (JsonValue.IsValid() && JsonValue->Type == EJson::Number)
        {
            const double Number = JsonValue->AsNumber();
            if (!FMath::IsFinite(Number)
                || FMath::Abs(Number) > MaxExactCommandJsonInteger
                || Number != FMath::TruncToDouble(Number))
            {
                return false;
            }
            ParsedValue = static_cast<int64>(Number);
        }
        else
        {
            return false;
        }

        Int64Property->SetPropertyValue(Value, ParsedValue);
        return true;
    }
}

bool FAkUGCCommandJson::Serialize(
    const FAkUGCCommandTransaction& Transaction,
    FString& OutJson,
    FString* OutError)
{
    OutJson.Reset();
    const FJsonObjectConverter::CustomExportCallback ExportCallback =
        FJsonObjectConverter::CustomExportCallback::CreateStatic(&ExportCommandProperty);
    const bool bSucceeded = FJsonObjectConverter::UStructToJsonObjectString(
        Transaction,
        OutJson,
        0,
        0,
        0,
        &ExportCallback);
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
    const FJsonObjectConverter::CustomImportCallback ImportCallback =
        FJsonObjectConverter::CustomImportCallback::CreateStatic(&ImportCommandProperty);
    const bool bSucceeded = FJsonObjectConverter::JsonObjectStringToUStruct(
        Json,
        &OutTransaction,
        0,
        0,
        false,
        nullptr,
        &ImportCallback);
    if (!bSucceeded && OutError)
    {
        *OutError = TEXT("Failed to deserialize UGC command transaction.");
    }
    return bSucceeded;
}

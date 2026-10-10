#include "Command/AkUGCCommandJson.h"

#include "Command/AkUGCCommand.h"
#include "Document/AkUGCDocumentJson.h"
#include "Dom/JsonObject.h"
#include "JsonObjectConverter.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
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
    if (!FAkUGCDocumentJson::ValidateNoDuplicateFields(Json, OutError))
    {
        return false;
    }
    TSharedPtr<FJsonObject> Root;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
    {
        if (OutError)
        {
            *OutError = TEXT("Failed to parse UGC command transaction JSON.");
        }
        return false;
    }
    const TArray<TSharedPtr<FJsonValue>>* Commands = nullptr;
    if (!Root->TryGetArrayField(TEXT("commands"), Commands) || !Commands)
    {
        if (OutError)
        {
            *OutError = TEXT("commands: Field must be an array.");
        }
        return false;
    }
    for (int32 Index = 0; Index < Commands->Num(); ++Index)
    {
        const TSharedPtr<FJsonValue>& Value = (*Commands)[Index];
        if (!Value.IsValid() || Value->Type != EJson::Object || !Value->AsObject().IsValid())
        {
            if (OutError)
            {
                *OutError = FString::Printf(TEXT("commands[%d]: Command must be an object."), Index);
            }
            return false;
        }
        for (const auto& Pair : Value->AsObject()->Values)
        {
            const FString Key(Pair.Key.ToView());
            if (!Key.Equals(TEXT("initialResources"), ESearchCase::IgnoreCase))
            {
                continue;
            }
            const double Resources = Pair.Value.IsValid() && Pair.Value->Type == EJson::Number
                ? Pair.Value->AsNumber() : -2.0;
            if (Key != TEXT("initialResources")
                || !FMath::IsFinite(Resources)
                || Resources != FMath::TruncToDouble(Resources)
                || Resources < INDEX_NONE
                || Resources > AkUGCTowerDefenseRulesetLimits::MaxInitialResources)
            {
                if (OutError)
                {
                    *OutError = FString::Printf(
                        TEXT("commands[%d].initialResources: Expected canonical integer from 0 to 1000000 or -1 to preserve resources."), Index);
                }
                return false;
            }
        }
    }
    const FJsonObjectConverter::CustomImportCallback ImportCallback =
        FJsonObjectConverter::CustomImportCallback::CreateStatic(&ImportCommandProperty);
    FAkUGCCommandTransaction ParsedTransaction;
    const bool bSucceeded = FJsonObjectConverter::JsonObjectStringToUStruct(
        Json,
        &ParsedTransaction,
        0,
        0,
        false,
        nullptr,
        &ImportCallback);
    if (!bSucceeded && OutError)
    {
        *OutError = TEXT("Failed to deserialize UGC command transaction.");
    }
    if (bSucceeded)
    {
        OutTransaction = MoveTemp(ParsedTransaction);
    }
    return bSucceeded;
}

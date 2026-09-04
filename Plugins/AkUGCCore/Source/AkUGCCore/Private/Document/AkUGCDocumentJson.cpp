#include "Document/AkUGCDocumentJson.h"

#include "Document/AkUGCDocument.h"
#include "Document/AkUGCDocumentMigration.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "JsonObjectConverter.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "String/LexFromString.h"
#include "UObject/UnrealType.h"
#include "Validation/AkUGCDocumentValidator.h"

namespace
{
    constexpr double MaxExactJsonInteger = 9007199254740991.0;

    TSharedPtr<FJsonValue> ExportDocumentProperty(FProperty* Property, const void* Value)
    {
        if (Property->GetFName() == GET_MEMBER_NAME_CHECKED(FAkUGCValue, IntegerValue)
            && CastField<FInt64Property>(Property))
        {
            return MakeShared<FJsonValueString>(LexToString(*static_cast<const int64*>(Value)));
        }
        return nullptr;
    }

    bool ImportDocumentProperty(
        const TSharedPtr<FJsonValue>& JsonValue,
        FProperty* Property,
        void* Value)
    {
        if (Property->GetFName() != GET_MEMBER_NAME_CHECKED(FAkUGCValue, IntegerValue)
            || !CastField<FInt64Property>(Property))
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
                || FMath::Abs(Number) > MaxExactJsonInteger
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

        static_cast<FInt64Property*>(Property)->SetPropertyValue(Value, ParsedValue);
        return true;
    }

    bool ValidateIntegerValueFields(
        const TSharedRef<FJsonObject>& Object,
        const FString& Path,
        FString& OutError)
    {
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
        {
            const FString FieldPath = Path.IsEmpty() ? Pair.Key : Path + TEXT(".") + Pair.Key;
            if (Pair.Key.Equals(TEXT("integerValue"), ESearchCase::IgnoreCase))
            {
                if (Pair.Key != TEXT("integerValue"))
                {
                    OutError = FString::Printf(
                        TEXT("%s: Field must use canonical casing 'integerValue'."),
                        *FieldPath);
                    return false;
                }
                if (!Pair.Value.IsValid())
                {
                    OutError = FString::Printf(TEXT("%s: Integer value is invalid."), *FieldPath);
                    return false;
                }

                int64 ParsedValue = 0;
                if (Pair.Value->Type == EJson::String)
                {
                    if (!LexTryParseString(ParsedValue, *Pair.Value->AsString()))
                    {
                        OutError = FString::Printf(TEXT("%s: Integer string is outside the int64 range."), *FieldPath);
                        return false;
                    }
                }
                else if (Pair.Value->Type == EJson::Number)
                {
                    const double Number = Pair.Value->AsNumber();
                    if (!FMath::IsFinite(Number)
                        || FMath::Abs(Number) > MaxExactJsonInteger
                        || Number != FMath::TruncToDouble(Number))
                    {
                        OutError = FString::Printf(
                            TEXT("%s: Numeric integer must be exactly representable; use a decimal string for large int64 values."),
                            *FieldPath);
                        return false;
                    }
                }
                else
                {
                    OutError = FString::Printf(TEXT("%s: Integer value must be a number or decimal string."), *FieldPath);
                    return false;
                }
            }

            if (Pair.Value.IsValid() && Pair.Value->Type == EJson::Object)
            {
                const TSharedPtr<FJsonObject> ChildObject = Pair.Value->AsObject();
                if (!ChildObject.IsValid() || !ValidateIntegerValueFields(ChildObject.ToSharedRef(), FieldPath, OutError))
                {
                    if (OutError.IsEmpty())
                    {
                        OutError = FString::Printf(TEXT("%s: JSON object is invalid."), *FieldPath);
                    }
                    return false;
                }
            }
            else if (Pair.Value.IsValid() && Pair.Value->Type == EJson::Array)
            {
                const TArray<TSharedPtr<FJsonValue>>& Array = Pair.Value->AsArray();
                for (int32 Index = 0; Index < Array.Num(); ++Index)
                {
                    if (Array[Index].IsValid() && Array[Index]->Type == EJson::Object)
                    {
                        const TSharedPtr<FJsonObject> ChildObject = Array[Index]->AsObject();
                        const FString ChildPath = FString::Printf(TEXT("%s[%d]"), *FieldPath, Index);
                        if (!ChildObject.IsValid()
                            || !ValidateIntegerValueFields(ChildObject.ToSharedRef(), ChildPath, OutError))
                        {
                            if (OutError.IsEmpty())
                            {
                                OutError = FString::Printf(TEXT("%s: JSON object is invalid."), *ChildPath);
                            }
                            return false;
                        }
                    }
                }
            }
        }
        return true;
    }
}

bool FAkUGCDocumentJson::Serialize(
    const FAkUGCProjectDocument& Document,
    FString& OutJson,
    FString* OutError)
{
    OutJson.Reset();
    if (Document.Manifest.SchemaVersion != AkUGCSchema::CurrentProjectDocumentVersion)
    {
        if (OutError)
        {
            *OutError = FString::Printf(
                TEXT("Cannot serialize project schema version %d; expected version %d."),
                Document.Manifest.SchemaVersion,
                AkUGCSchema::CurrentProjectDocumentVersion);
        }
        return false;
    }

    const FAkUGCValidationResult Validation = FAkUGCDocumentValidator::Validate(Document);
    if (!Validation.IsValid())
    {
        if (OutError)
        {
            const FAkUGCValidationIssue* FirstError = Validation.Issues.FindByPredicate([](const FAkUGCValidationIssue& Issue)
            {
                return Issue.Severity == EAkUGCValidationSeverity::Error;
            });
            *OutError = FirstError
                ? FString::Printf(TEXT("%s: %s"), *FirstError->Path, *FirstError->Message)
                : TEXT("UGC project document validation failed.");
        }
        return false;
    }

    const FJsonObjectConverter::CustomExportCallback ExportCallback =
        FJsonObjectConverter::CustomExportCallback::CreateStatic(&ExportDocumentProperty);
    const bool bSucceeded = FJsonObjectConverter::UStructToJsonObjectString(
        Document,
        OutJson,
        0,
        0,
        0,
        &ExportCallback);
    if (!bSucceeded && OutError)
    {
        *OutError = TEXT("Failed to serialize UGC project document.");
    }
    return bSucceeded;
}

bool FAkUGCDocumentJson::Deserialize(
    const FString& Json,
    FAkUGCProjectDocument& OutDocument,
    FString* OutError,
    FAkUGCDocumentMigrationResult* OutMigration)
{
    OutDocument = FAkUGCProjectDocument{};
    if (OutMigration)
    {
        *OutMigration = FAkUGCDocumentMigrationResult{};
    }

    TSharedPtr<FJsonObject> RootObject;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
    if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
    {
        if (OutError)
        {
            *OutError = TEXT("Failed to parse UGC project document JSON.");
        }
        return false;
    }

    const FAkUGCDocumentMigrationResult Migration = FAkUGCDocumentMigrator::Migrate(RootObject.ToSharedRef());
    if (OutMigration)
    {
        *OutMigration = Migration;
    }
    if (!Migration.bSucceeded)
    {
        if (OutError)
        {
            *OutError = Migration.ErrorPath.IsEmpty()
                ? Migration.ErrorMessage
                : FString::Printf(TEXT("%s: %s"), *Migration.ErrorPath, *Migration.ErrorMessage);
        }
        return false;
    }

    FString IntegerValidationError;
    if (!ValidateIntegerValueFields(RootObject.ToSharedRef(), FString{}, IntegerValidationError))
    {
        if (OutError)
        {
            *OutError = MoveTemp(IntegerValidationError);
        }
        return false;
    }

    FAkUGCProjectDocument MigratedDocument;
    FText ConversionError;
    const FJsonObjectConverter::CustomImportCallback ImportCallback =
        FJsonObjectConverter::CustomImportCallback::CreateStatic(&ImportDocumentProperty);
    if (!FJsonObjectConverter::JsonObjectToUStruct(
        RootObject.ToSharedRef(),
        &MigratedDocument,
        0,
        0,
        false,
        &ConversionError,
        &ImportCallback))
    {
        if (OutError)
        {
            *OutError = ConversionError.IsEmpty()
                ? TEXT("Failed to deserialize UGC project document.")
                : ConversionError.ToString();
        }
        return false;
    }

    OutDocument = MoveTemp(MigratedDocument);
    return true;
}

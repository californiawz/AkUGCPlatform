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
#include "Validation/AkUGCInputBudget.h"

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
        for (const auto& Pair : Object->Values)
        {
            const FString Key(Pair.Key.ToView());
            const FString FieldPath = Path.IsEmpty() ? Key : Path + TEXT(".") + Key;
            if (Key.Equals(TEXT("integerValue"), ESearchCase::IgnoreCase))
            {
                if (Key != TEXT("integerValue"))
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

    bool ValidateExactFields(
        const TSharedRef<FJsonObject>& Object,
        const TSet<FString>& ExpectedFields,
        const FString& Path,
        FString& OutError)
    {
        for (const auto& Pair : Object->Values)
        {
            const FString Key(Pair.Key.ToView());
            bool bExactMatch = false;
            FString CanonicalField;
            for (const FString& Field : ExpectedFields)
            {
                if (Field.Equals(Key, ESearchCase::CaseSensitive))
                {
                    bExactMatch = true;
                    break;
                }
                if (Field.Equals(Key, ESearchCase::IgnoreCase))
                {
                    CanonicalField = Field;
                }
            }
            if (bExactMatch)
            {
                continue;
            }
            OutError = !CanonicalField.IsEmpty()
                ? FString::Printf(TEXT("%s.%s: Field must use canonical casing '%s'."), *Path, *Key, *CanonicalField)
                : FString::Printf(TEXT("%s.%s: Field is not supported."), *Path, *Key);
            return false;
        }
        for (const FString& Field : ExpectedFields)
        {
            bool bFoundExact = false;
            for (const auto& Pair : Object->Values)
            {
                const FString Key(Pair.Key.ToView());
                if (Key.Equals(Field, ESearchCase::CaseSensitive))
                {
                    bFoundExact = true;
                    break;
                }
            }
            if (!bFoundExact)
            {
                OutError = FString::Printf(TEXT("%s.%s: Field is required."), *Path, *Field);
                return false;
            }
        }
        return true;
    }

    bool ValidateCurrentRulesetJson(const TSharedRef<FJsonObject>& RootObject, FString& OutError)
    {
        const TArray<TSharedPtr<FJsonValue>>* Scenes = nullptr;
        if (!RootObject->TryGetArrayField(TEXT("scenes"), Scenes) || !Scenes)
        {
            OutError = TEXT("scenes: Field must be an array.");
            return false;
        }

        const TSet<FString> RulesetFields = {
            TEXT("waves"),
            TEXT("initialResources"),
            TEXT("waveIntervalSeconds"),
            TEXT("defeatCondition"),
            TEXT("victoryCondition")};
        const TSet<FString> WaveFields = {
            TEXT("waveId"),
            TEXT("spawnPointEntityId"),
            TEXT("startDelaySeconds")};

        for (int32 SceneIndex = 0; SceneIndex < Scenes->Num(); ++SceneIndex)
        {
            if (!(*Scenes)[SceneIndex].IsValid() || (*Scenes)[SceneIndex]->Type != EJson::Object)
            {
                OutError = FString::Printf(TEXT("scenes[%d]: Scene must be an object."), SceneIndex);
                return false;
            }
            const TSharedPtr<FJsonObject> Scene = (*Scenes)[SceneIndex]->AsObject();
            const TSharedPtr<FJsonObject>* Ruleset = nullptr;
            const FString RulesetPath = FString::Printf(TEXT("scenes[%d].ruleset"), SceneIndex);
            if (!Scene.IsValid()
                || !Scene->TryGetObjectField(TEXT("ruleset"), Ruleset)
                || !Ruleset
                || !Ruleset->IsValid())
            {
                OutError = RulesetPath + TEXT(": Ruleset must be an object.");
                return false;
            }
            if (!ValidateExactFields(Ruleset->ToSharedRef(), RulesetFields, RulesetPath, OutError))
            {
                return false;
            }

            const TArray<TSharedPtr<FJsonValue>>* Waves = nullptr;
            if (!(*Ruleset)->TryGetArrayField(TEXT("waves"), Waves) || !Waves)
            {
                OutError = RulesetPath + TEXT(".waves: Field must be an array.");
                return false;
            }
            double InitialResources = 0.0;
            const TSharedPtr<FJsonValue> ResourcesValue = (*Ruleset)->TryGetField(TEXT("initialResources"));
            if (!ResourcesValue.IsValid() || ResourcesValue->Type != EJson::Number
                || !(*Ruleset)->TryGetNumberField(TEXT("initialResources"), InitialResources)
                || !FMath::IsFinite(InitialResources)
                || InitialResources != FMath::TruncToDouble(InitialResources)
                || InitialResources < 0.0
                || InitialResources > AkUGCTowerDefenseRulesetLimits::MaxInitialResources)
            {
                OutError = RulesetPath + TEXT(".initialResources: Field must be an integer from 0 to 1000000.");
                return false;
            }
            double WaveIntervalSeconds = 0.0;
            if (!(*Ruleset)->TryGetNumberField(TEXT("waveIntervalSeconds"), WaveIntervalSeconds)
                || !FMath::IsFinite(WaveIntervalSeconds))
            {
                OutError = RulesetPath + TEXT(".waveIntervalSeconds: Field must be a finite number.");
                return false;
            }
            FString DefeatCondition;
            if (!(*Ruleset)->TryGetStringField(TEXT("defeatCondition"), DefeatCondition)
                || DefeatCondition != TEXT("BaseHealthDepleted"))
            {
                OutError = RulesetPath + TEXT(".defeatCondition: Expected 'BaseHealthDepleted'.");
                return false;
            }
            FString VictoryCondition;
            if (!(*Ruleset)->TryGetStringField(TEXT("victoryCondition"), VictoryCondition)
                || VictoryCondition != TEXT("AllWavesCleared"))
            {
                OutError = RulesetPath + TEXT(".victoryCondition: Expected 'AllWavesCleared'.");
                return false;
            }

            for (int32 WaveIndex = 0; WaveIndex < Waves->Num(); ++WaveIndex)
            {
                if (!(*Waves)[WaveIndex].IsValid() || (*Waves)[WaveIndex]->Type != EJson::Object)
                {
                    OutError = FString::Printf(TEXT("%s.waves[%d]: Wave must be an object."), *RulesetPath, WaveIndex);
                    return false;
                }
                const TSharedPtr<FJsonObject> Wave = (*Waves)[WaveIndex]->AsObject();
                const FString WavePath = FString::Printf(TEXT("%s.waves[%d]"), *RulesetPath, WaveIndex);
                if (!Wave.IsValid() || !ValidateExactFields(Wave.ToSharedRef(), WaveFields, WavePath, OutError))
                {
                    return false;
                }
                FString GuidValue;
                if (!Wave->TryGetStringField(TEXT("waveId"), GuidValue))
                {
                    OutError = WavePath + TEXT(".waveId: Field must be a GUID string.");
                    return false;
                }
                if (!Wave->TryGetStringField(TEXT("spawnPointEntityId"), GuidValue))
                {
                    OutError = WavePath + TEXT(".spawnPointEntityId: Field must be a GUID string.");
                    return false;
                }
                double StartDelaySeconds = 0.0;
                if (!Wave->TryGetNumberField(TEXT("startDelaySeconds"), StartDelaySeconds)
                    || !FMath::IsFinite(StartDelaySeconds))
                {
                    OutError = WavePath + TEXT(".startDelaySeconds: Field must be a finite number.");
                    return false;
                }
            }
        }
        return true;
    }
}

bool FAkUGCDocumentJson::ValidateNoDuplicateFields(const FString& Json, FString* OutError)
{
    return AkUGCInputBudget::ValidateJson(
        Json,
        AkUGCInputBudget::MaxLogicPackBytes,
        TEXT("JSON input"),
        OutError);
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

    // 序列化只负责把内存结构转成 JSON，不做业务校验。
    // 校验在显式的校验点（OpenDocument / LoadProjectJson / SaveProject 前）进行，
    // 这样导出与诊断场景可以序列化尚未通过校验的草稿或恶意 fixture。
    const FJsonObjectConverter::CustomExportCallback ExportCallback =
        FJsonObjectConverter::CustomExportCallback::CreateStatic(&ExportDocumentProperty);
    const bool bSucceeded = FJsonObjectConverter::UStructToJsonObjectString(
        Document,
        OutJson,
        0,
        0,
        0,
        &ExportCallback);
    if (!bSucceeded)
    {
        if (OutError)
        {
            *OutError = TEXT("Failed to serialize UGC project document.");
        }
        return false;
    }
    if (!AkUGCInputBudget::ValidateUtf8Text(
            OutJson,
            AkUGCInputBudget::MaxProjectDocumentBytes,
            TEXT("UGC project document"),
            OutError))
    {
        OutJson.Reset();
        return false;
    }
    return true;
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

    if (!AkUGCInputBudget::ValidateJson(
            Json,
            AkUGCInputBudget::MaxProjectDocumentBytes,
            TEXT("UGC project document"),
            OutError))
    {
        return false;
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

    FString RulesetValidationError;
    if (!ValidateCurrentRulesetJson(RootObject.ToSharedRef(), RulesetValidationError))
    {
        if (OutError)
        {
            *OutError = MoveTemp(RulesetValidationError);
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

    const FAkUGCValidationResult InputBudgetValidation =
        FAkUGCDocumentValidator::ValidateInputBudgets(MigratedDocument);
    if (!InputBudgetValidation.IsValid())
    {
        if (OutError)
        {
            const FAkUGCValidationIssue* FirstError =
                InputBudgetValidation.Issues.FindByPredicate(
                    [](const FAkUGCValidationIssue& Issue)
                    {
                        return Issue.Severity == EAkUGCValidationSeverity::Error;
                    });
            *OutError = FirstError
                ? FString::Printf(
                    TEXT("%s: %s"),
                    *FirstError->Path,
                    *FirstError->Message)
                : TEXT("UGC project document exceeds an input budget.");
        }
        return false;
    }

    OutDocument = MoveTemp(MigratedDocument);
    return true;
}

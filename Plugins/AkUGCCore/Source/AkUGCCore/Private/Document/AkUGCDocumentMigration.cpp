#include "Document/AkUGCDocumentMigration.h"

#include "Document/AkUGCDocument.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
    FAkUGCDocumentMigrationResult Failure(
        int32 SourceVersion,
        FString Path,
        FString Message)
    {
        FAkUGCDocumentMigrationResult Result;
        Result.SourceVersion = SourceVersion;
        Result.TargetVersion = AkUGCSchema::CurrentProjectDocumentVersion;
        Result.ErrorPath = MoveTemp(Path);
        Result.ErrorMessage = MoveTemp(Message);
        return Result;
    }

    bool FindCanonicalField(
        const TSharedRef<FJsonObject>& Object,
        const TCHAR* FieldName,
        bool bRequired,
        TSharedPtr<FJsonValue>& OutValue,
        FString& OutError)
    {
        OutValue.Reset();
        for (const auto& Pair : Object->Values)
        {
            const FString Key(Pair.Key.ToView());
            if (!Key.Equals(FieldName, ESearchCase::IgnoreCase))
            {
                continue;
            }
            if (!Key.Equals(FieldName, ESearchCase::CaseSensitive))
            {
                OutError = FString::Printf(
                    TEXT("Field '%s' must use canonical casing '%s'."),
                    *Key,
                    FieldName);
                return false;
            }
            if (OutValue.IsValid())
            {
                OutError = FString::Printf(TEXT("Field '%s' is duplicated."), FieldName);
                return false;
            }
            OutValue = Pair.Value;
        }

        if (bRequired && !OutValue.IsValid())
        {
            OutError = FString::Printf(TEXT("Field '%s' is required."), FieldName);
            return false;
        }
        return true;
    }

    bool ReadIntegerVersion(
        const TSharedRef<FJsonObject>& Object,
        const TCHAR* FieldName,
        int32 MissingValue,
        int32& OutVersion,
        FString& OutError)
    {
        TSharedPtr<FJsonValue> Value;
        if (!FindCanonicalField(Object, FieldName, false, Value, OutError))
        {
            return false;
        }
        if (!Value.IsValid())
        {
            OutVersion = MissingValue;
            return true;
        }
        if (Value->Type != EJson::Number)
        {
            OutError = TEXT("Schema version must be an integer.");
            return false;
        }

        const double Number = Value->AsNumber();
        if (!FMath::IsFinite(Number)
            || Number < static_cast<double>(MIN_int32)
            || Number > static_cast<double>(MAX_int32)
            || Number != FMath::TruncToDouble(Number))
        {
            OutError = TEXT("Schema version must be a finite 32-bit integer.");
            return false;
        }
        OutVersion = static_cast<int32>(Number);
        return true;
    }

    bool ReadObjectArray(
        const TSharedRef<FJsonObject>& Object,
        const TCHAR* FieldName,
        const FString& Path,
        TArray<TSharedPtr<FJsonObject>>& OutObjects,
        FString& OutPath,
        FString& OutError)
    {
        TSharedPtr<FJsonValue> ArrayValue;
        if (!FindCanonicalField(Object, FieldName, false, ArrayValue, OutError))
        {
            OutPath = Path;
            return false;
        }
        if (!ArrayValue.IsValid())
        {
            return true;
        }
        if (ArrayValue->Type != EJson::Array)
        {
            OutPath = Path;
            OutError = TEXT("Field must be an array.");
            return false;
        }

        const TArray<TSharedPtr<FJsonValue>>& Values = ArrayValue->AsArray();
        OutObjects.Reserve(Values.Num());
        for (int32 Index = 0; Index < Values.Num(); ++Index)
        {
            if (!Values[Index].IsValid() || Values[Index]->Type != EJson::Object)
            {
                OutPath = FString::Printf(TEXT("%s[%d]"), *Path, Index);
                OutError = TEXT("Array element must be an object.");
                return false;
            }
            const TSharedPtr<FJsonObject> ChildObject = Values[Index]->AsObject();
            if (!ChildObject.IsValid())
            {
                OutPath = FString::Printf(TEXT("%s[%d]"), *Path, Index);
                OutError = TEXT("Array element contains an invalid object.");
                return false;
            }
            OutObjects.Add(ChildObject);
        }
        return true;
    }

    bool CollectLegacyComponentsToUpgrade(
        const TSharedRef<FJsonObject>& RootObject,
        TArray<TSharedPtr<FJsonObject>>& OutComponents,
        FString& OutPath,
        FString& OutError)
    {
        TArray<TSharedPtr<FJsonObject>> Scenes;
        if (!ReadObjectArray(RootObject, TEXT("scenes"), TEXT("scenes"), Scenes, OutPath, OutError))
        {
            return false;
        }

        for (int32 SceneIndex = 0; SceneIndex < Scenes.Num(); ++SceneIndex)
        {
            TArray<TSharedPtr<FJsonObject>> Entities;
            const FString EntitiesPath = FString::Printf(TEXT("scenes[%d].entities"), SceneIndex);
            if (!ReadObjectArray(Scenes[SceneIndex].ToSharedRef(), TEXT("entities"), EntitiesPath, Entities, OutPath, OutError))
            {
                return false;
            }

            for (int32 EntityIndex = 0; EntityIndex < Entities.Num(); ++EntityIndex)
            {
                TArray<TSharedPtr<FJsonObject>> Components;
                const FString ComponentsPath = FString::Printf(
                    TEXT("scenes[%d].entities[%d].components"),
                    SceneIndex,
                    EntityIndex);
                if (!ReadObjectArray(
                    Entities[EntityIndex].ToSharedRef(),
                    TEXT("components"),
                    ComponentsPath,
                    Components,
                    OutPath,
                    OutError))
                {
                    return false;
                }

                for (int32 ComponentIndex = 0; ComponentIndex < Components.Num(); ++ComponentIndex)
                {
                    int32 ComponentVersion = 0;
                    if (!ReadIntegerVersion(
                        Components[ComponentIndex].ToSharedRef(),
                        TEXT("schemaVersion"),
                        0,
                        ComponentVersion,
                        OutError))
                    {
                        OutPath = FString::Printf(TEXT("%s[%d].schemaVersion"), *ComponentsPath, ComponentIndex);
                        return false;
                    }
                    if (ComponentVersion < 0)
                    {
                        OutPath = FString::Printf(TEXT("%s[%d].schemaVersion"), *ComponentsPath, ComponentIndex);
                        OutError = TEXT("Component schema version cannot be negative.");
                        return false;
                    }
                    if (ComponentVersion == 0)
                    {
                        OutComponents.Add(Components[ComponentIndex]);
                    }
                }
            }
        }
        return true;
    }
}

FAkUGCDocumentMigrationResult FAkUGCDocumentMigrator::Migrate(
    const TSharedRef<FJsonObject>& RootObject)
{
    TSharedPtr<FJsonValue> ManifestValue;
    FString FieldError;
    if (!FindCanonicalField(RootObject, TEXT("manifest"), true, ManifestValue, FieldError))
    {
        return Failure(INDEX_NONE, TEXT("manifest"), MoveTemp(FieldError));
    }
    if (ManifestValue->Type != EJson::Object || !ManifestValue->AsObject().IsValid())
    {
        return Failure(INDEX_NONE, TEXT("manifest"), TEXT("Project manifest must be an object."));
    }

    const TSharedRef<FJsonObject> Manifest = ManifestValue->AsObject().ToSharedRef();
    int32 SourceVersion = 0;
    FString VersionError;
    if (!ReadIntegerVersion(Manifest, TEXT("schemaVersion"), 0, SourceVersion, VersionError))
    {
        return Failure(INDEX_NONE, TEXT("manifest.schemaVersion"), MoveTemp(VersionError));
    }
    if (SourceVersion < AkUGCSchema::OldestSupportedProjectDocumentVersion)
    {
        return Failure(
            SourceVersion,
            TEXT("manifest.schemaVersion"),
            FString::Printf(TEXT("Project schema version %d is not supported."), SourceVersion));
    }
    if (SourceVersion > AkUGCSchema::CurrentProjectDocumentVersion)
    {
        return Failure(
            SourceVersion,
            TEXT("manifest.schemaVersion"),
            FString::Printf(
                TEXT("Project schema version %d is newer than supported version %d."),
                SourceVersion,
                AkUGCSchema::CurrentProjectDocumentVersion));
    }

    FAkUGCDocumentMigrationResult Result;
    Result.SourceVersion = SourceVersion;
    Result.TargetVersion = AkUGCSchema::CurrentProjectDocumentVersion;

    TArray<TSharedPtr<FJsonObject>> ComponentsToUpgrade;
    FString ErrorPath;
    FString ErrorMessage;
    if (!CollectLegacyComponentsToUpgrade(RootObject, ComponentsToUpgrade, ErrorPath, ErrorMessage))
    {
        return Failure(SourceVersion, MoveTemp(ErrorPath), MoveTemp(ErrorMessage));
    }

    TArray<TSharedPtr<FJsonObject>> ScenesToInitializeLogicGraph;
    if (SourceVersion <= 1)
    {
        TArray<TSharedPtr<FJsonObject>> Scenes;
        if (!ReadObjectArray(RootObject, TEXT("scenes"), TEXT("scenes"), Scenes, ErrorPath, ErrorMessage))
        {
            return Failure(SourceVersion, MoveTemp(ErrorPath), MoveTemp(ErrorMessage));
        }
        for (int32 SceneIndex = 0; SceneIndex < Scenes.Num(); ++SceneIndex)
        {
            TSharedPtr<FJsonValue> LogicGraphValue;
            if (!FindCanonicalField(Scenes[SceneIndex].ToSharedRef(), TEXT("logicGraph"), false, LogicGraphValue, ErrorMessage))
            {
                return Failure(
                    SourceVersion,
                    FString::Printf(TEXT("scenes[%d].logicGraph"), SceneIndex),
                    MoveTemp(ErrorMessage));
            }
            if (!LogicGraphValue.IsValid())
            {
                ScenesToInitializeLogicGraph.Add(Scenes[SceneIndex]);
            }
            else if (LogicGraphValue->Type != EJson::Object || !LogicGraphValue->AsObject().IsValid())
            {
                return Failure(
                    SourceVersion,
                    FString::Printf(TEXT("scenes[%d].logicGraph"), SceneIndex),
                    TEXT("Logic graph must be an object."));
            }
        }
    }

    TArray<TSharedPtr<FJsonObject>> ScenesToInitializeRuleset;
    TArray<TSharedPtr<FJsonObject>> RulesetScenes;
    if (!ReadObjectArray(RootObject, TEXT("scenes"), TEXT("scenes"), RulesetScenes, ErrorPath, ErrorMessage))
    {
        return Failure(SourceVersion, MoveTemp(ErrorPath), MoveTemp(ErrorMessage));
    }
    for (int32 SceneIndex = 0; SceneIndex < RulesetScenes.Num(); ++SceneIndex)
    {
        TSharedPtr<FJsonValue> RulesetValue;
        if (!FindCanonicalField(RulesetScenes[SceneIndex].ToSharedRef(), TEXT("ruleset"), false, RulesetValue, ErrorMessage))
        {
            return Failure(
                SourceVersion,
                FString::Printf(TEXT("scenes[%d].ruleset"), SceneIndex),
                MoveTemp(ErrorMessage));
        }
        if (!RulesetValue.IsValid())
        {
            if (SourceVersion <= 3)
            {
                ScenesToInitializeRuleset.Add(RulesetScenes[SceneIndex]);
            }
            else
            {
                return Failure(
                    SourceVersion,
                    FString::Printf(TEXT("scenes[%d].ruleset"), SceneIndex),
                    TEXT("V4 scene requires a tower defense ruleset object."));
            }
        }
        else if (RulesetValue->Type != EJson::Object || !RulesetValue->AsObject().IsValid())
        {
            return Failure(
                SourceVersion,
                FString::Printf(TEXT("scenes[%d].ruleset"), SceneIndex),
                TEXT("Tower defense ruleset must be an object."));
        }
    }

    int32 WorkingVersion = SourceVersion;
    if (WorkingVersion == 0)
    {
        for (const TSharedPtr<FJsonObject>& Component : ComponentsToUpgrade)
        {
            Component->SetNumberField(TEXT("schemaVersion"), 1);
        }
        Manifest->SetNumberField(TEXT("schemaVersion"), 1);
        Result.AppliedSteps.Add(TEXT("ProjectDocumentV0ToV1"));
        WorkingVersion = 1;
    }
    if (WorkingVersion == 1)
    {
        for (const TSharedPtr<FJsonObject>& Scene : ScenesToInitializeLogicGraph)
        {
            TSharedRef<FJsonObject> LogicGraph = MakeShared<FJsonObject>();
            LogicGraph->SetArrayField(TEXT("nodes"), TArray<TSharedPtr<FJsonValue>>{});
            LogicGraph->SetArrayField(TEXT("connections"), TArray<TSharedPtr<FJsonValue>>{});
            Scene->SetObjectField(TEXT("logicGraph"), LogicGraph);
        }
        Manifest->SetNumberField(TEXT("schemaVersion"), 2);
        Result.AppliedSteps.Add(TEXT("ProjectDocumentV1ToV2"));
        WorkingVersion = 2;
    }
    if (WorkingVersion == 2)
    {
        Manifest->SetNumberField(TEXT("schemaVersion"), 3);
        Result.AppliedSteps.Add(TEXT("ProjectDocumentV2ToV3"));
        WorkingVersion = 3;
    }
    if (WorkingVersion == 3)
    {
        for (const TSharedPtr<FJsonObject>& Scene : ScenesToInitializeRuleset)
        {
            TSharedRef<FJsonObject> Ruleset = MakeShared<FJsonObject>();
            Ruleset->SetArrayField(TEXT("waves"), TArray<TSharedPtr<FJsonValue>>{});
            Ruleset->SetNumberField(TEXT("waveIntervalSeconds"), 5.0);
            Ruleset->SetStringField(TEXT("defeatCondition"), TEXT("BaseHealthDepleted"));
            Ruleset->SetStringField(TEXT("victoryCondition"), TEXT("AllWavesCleared"));
            Scene->SetObjectField(TEXT("ruleset"), Ruleset);
        }
        Manifest->SetNumberField(TEXT("schemaVersion"), 4);
        Result.AppliedSteps.Add(TEXT("ProjectDocumentV3ToV4"));
        WorkingVersion = 4;
    }
    if (WorkingVersion == 4)
    {
        TArray<TSharedPtr<FJsonObject>> Scenes;
        if (!ReadObjectArray(RootObject, TEXT("scenes"), TEXT("scenes"), Scenes, ErrorPath, ErrorMessage))
        {
            return Failure(SourceVersion, MoveTemp(ErrorPath), MoveTemp(ErrorMessage));
        }
        for (int32 SceneIndex = 0; SceneIndex < Scenes.Num(); ++SceneIndex)
        {
            TSharedPtr<FJsonValue> LogicGraphValue;
            const FString LogicGraphPath = FString::Printf(TEXT("scenes[%d].logicGraph"), SceneIndex);
            if (!FindCanonicalField(Scenes[SceneIndex].ToSharedRef(), TEXT("logicGraph"), false, LogicGraphValue, ErrorMessage))
            {
                return Failure(SourceVersion, LogicGraphPath, MoveTemp(ErrorMessage));
            }
            if (!LogicGraphValue.IsValid())
            {
                continue;
            }
            if (LogicGraphValue->Type != EJson::Object || !LogicGraphValue->AsObject().IsValid())
            {
                return Failure(SourceVersion, LogicGraphPath, TEXT("Logic graph must be an object."));
            }

            TArray<TSharedPtr<FJsonObject>> Nodes;
            const FString NodesPath = LogicGraphPath + TEXT(".nodes");
            if (!ReadObjectArray(LogicGraphValue->AsObject().ToSharedRef(), TEXT("nodes"), NodesPath, Nodes, ErrorPath, ErrorMessage))
            {
                return Failure(SourceVersion, MoveTemp(ErrorPath), MoveTemp(ErrorMessage));
            }
            for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
            {
                Nodes[NodeIndex]->SetNumberField(TEXT("positionX"), 0.0);
                Nodes[NodeIndex]->SetNumberField(TEXT("positionY"), NodeIndex * 180.0);
            }
        }
        Manifest->SetNumberField(TEXT("schemaVersion"), 5);
        Result.AppliedSteps.Add(TEXT("ProjectDocumentV4ToV5"));
        WorkingVersion = 5;
    }

    if (WorkingVersion != AkUGCSchema::CurrentProjectDocumentVersion)
    {
        return Failure(
            SourceVersion,
            TEXT("manifest.schemaVersion"),
            FString::Printf(
                TEXT("No migration path exists from version %d to version %d."),
                WorkingVersion,
                AkUGCSchema::CurrentProjectDocumentVersion));
    }

    Result.bSucceeded = true;
    return Result;
}

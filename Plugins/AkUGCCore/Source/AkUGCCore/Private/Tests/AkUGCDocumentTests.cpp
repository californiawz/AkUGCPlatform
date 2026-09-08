#include "Misc/AutomationTest.h"

#include "Document/AkUGCDocument.h"
#include "Document/AkUGCDocumentJson.h"
#include "Document/AkUGCDocumentMigration.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Validation/AkUGCDocumentValidator.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCDocumentRoundTripTest,
    "AkUGC.Core.Document.JsonRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCDocumentRoundTripTest::RunTest(const FString& Parameters)
{
    FAkUGCProjectDocument Source;
    Source.Manifest.ProjectId = FGuid::NewGuid();
    Source.Manifest.DisplayName = TEXT("Phase0 Tower Defense");
    Source.Manifest.TemplateId = TEXT("official.tower_defense");
    Source.Manifest.Capabilities = {TEXT("world.spawn"), TEXT("rules.wave")};

    FAkUGCSceneDocument& Scene = Source.Scenes.AddDefaulted_GetRef();
    Scene.SceneId = FGuid::NewGuid();
    Scene.DisplayName = TEXT("Main");

    FAkUGCEntityRecord& Entity = Scene.Entities.AddDefaulted_GetRef();
    Entity.EntityId = FGuid::NewGuid();
    Entity.PrefabId = TEXT("official.gameplay.base");
    Entity.Transform.SetLocation(FVector(100.0, 200.0, 0.0));

    FAkUGCComponentRecord& Health = Entity.Components.AddDefaulted_GetRef();
    Health.TypeId = TEXT("core.health");
    FAkUGCValue HealthValue;
    HealthValue.Type = EAkUGCValueType::Number;
    HealthValue.NumberValue = 1000.0;
    Health.Properties.Add(TEXT("maxHealth"), HealthValue);
    const FGuid BaseEntityId = Entity.EntityId;

    Scene.Ruleset.WaveIntervalSeconds = 7.5;
    for (int32 WaveIndex = 0; WaveIndex < AkUGCTowerDefenseRulesetLimits::RequiredWaveCount; ++WaveIndex)
    {
        FAkUGCEntityRecord& SpawnPoint = Scene.Entities.AddDefaulted_GetRef();
        SpawnPoint.EntityId = FGuid::NewGuid();
        SpawnPoint.PrefabId = TEXT("official.gameplay.enemy_spawn");

        FAkUGCTowerDefenseWave& Wave = Scene.Ruleset.Waves.AddDefaulted_GetRef();
        Wave.WaveId = FGuid::NewGuid();
        Wave.SpawnPointEntityId = SpawnPoint.EntityId;
        Wave.StartDelaySeconds = WaveIndex * 2.0;
    }

    const FAkUGCValidationResult SourceValidation = FAkUGCDocumentValidator::Validate(Source);
    TestTrue(TEXT("Source document is valid"), SourceValidation.IsValid());

    FString Json;
    FString Error;
    TestTrue(TEXT("Document serializes"), FAkUGCDocumentJson::Serialize(Source, Json, &Error));
    TestFalse(TEXT("Serialized JSON is not empty"), Json.IsEmpty());

    FAkUGCProjectDocument Restored;
    if (!FAkUGCDocumentJson::Deserialize(Json, Restored, &Error))
    {
        AddError(FString::Printf(TEXT("Document failed to deserialize: %s"), *Error));
        return false;
    }
    TestEqual(TEXT("Project ID round-trips"), Restored.Manifest.ProjectId, Source.Manifest.ProjectId);
    TestEqual(TEXT("Scene count round-trips"), Restored.Scenes.Num(), 1);
    if (Restored.Scenes.Num() != 1 || Restored.Scenes[0].Entities.IsEmpty())
    {
        AddError(TEXT("Restored document does not contain the expected entity."));
        return false;
    }
    TestEqual(TEXT("Entity ID round-trips"), Restored.Scenes[0].Entities[0].EntityId, BaseEntityId);
    TestEqual(TEXT("Prefab ID round-trips"), Restored.Scenes[0].Entities[0].PrefabId, FName(TEXT("official.gameplay.base")));
    TestEqual(TEXT("Component count round-trips"), Restored.Scenes[0].Entities[0].Components.Num(), 1);
    TestEqual(TEXT("Ruleset wave count round-trips"),
        Restored.Scenes[0].Ruleset.Waves.Num(),
        AkUGCTowerDefenseRulesetLimits::RequiredWaveCount);
    TestEqual(TEXT("Ruleset wave interval round-trips"), Restored.Scenes[0].Ruleset.WaveIntervalSeconds, 7.5);
    if (Restored.Scenes[0].Ruleset.Waves.Num() == AkUGCTowerDefenseRulesetLimits::RequiredWaveCount)
    {
        TestEqual(TEXT("Wave delay round-trips"), Restored.Scenes[0].Ruleset.Waves[2].StartDelaySeconds, 4.0);
        TestEqual(TEXT("Wave Spawn Point reference round-trips"),
            Restored.Scenes[0].Ruleset.Waves[1].SpawnPointEntityId,
            Source.Scenes[0].Ruleset.Waves[1].SpawnPointEntityId);
    }

    FAkUGCDocumentMigrationResult Migration;
    TestTrue(TEXT("Current document loads through migration boundary"), FAkUGCDocumentJson::Deserialize(Json, Restored, &Error, &Migration));
    TestEqual(TEXT("Current document source version is reported"), Migration.SourceVersion, AkUGCSchema::CurrentProjectDocumentVersion);
    TestTrue(TEXT("Current document migration is a no-op"), Migration.AppliedSteps.IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCDocumentDuplicateEntityTest,
    "AkUGC.Core.Document.RejectsDuplicateEntityIds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCDocumentDuplicateEntityTest::RunTest(const FString& Parameters)
{
    FAkUGCProjectDocument Document;
    Document.Manifest.ProjectId = FGuid::NewGuid();
    Document.Manifest.DisplayName = TEXT("Duplicate Test");
    Document.Manifest.TemplateId = TEXT("official.tower_defense");

    FAkUGCSceneDocument& Scene = Document.Scenes.AddDefaulted_GetRef();
    Scene.SceneId = FGuid::NewGuid();

    const FGuid DuplicateId = FGuid::NewGuid();
    FAkUGCEntityRecord First;
    First.EntityId = DuplicateId;
    First.PrefabId = TEXT("official.gameplay.base");
    Scene.Entities.Add(First);

    FAkUGCEntityRecord Second = First;
    Scene.Entities.Add(Second);

    const FAkUGCValidationResult Validation = FAkUGCDocumentValidator::Validate(Document);
    TestFalse(TEXT("Duplicate entity IDs are rejected"), Validation.IsValid());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCDocumentParentCycleTest,
    "AkUGC.Core.Document.RejectsParentCycles",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCDocumentParentCycleTest::RunTest(const FString& Parameters)
{
    FAkUGCProjectDocument Document;
    Document.Manifest.ProjectId = FGuid::NewGuid();
    Document.Manifest.DisplayName = TEXT("Parent Cycle Test");
    Document.Manifest.TemplateId = TEXT("official.tower_defense");

    FAkUGCSceneDocument& Scene = Document.Scenes.AddDefaulted_GetRef();
    Scene.SceneId = FGuid::NewGuid();

    FAkUGCEntityRecord First;
    First.EntityId = FGuid::NewGuid();
    First.PrefabId = TEXT("official.gameplay.base");

    FAkUGCEntityRecord Second;
    Second.EntityId = FGuid::NewGuid();
    Second.PrefabId = TEXT("official.gameplay.tower");

    First.ParentEntityId = Second.EntityId;
    Second.ParentEntityId = First.EntityId;
    Scene.Entities = {First, Second};

    const FAkUGCValidationResult Validation = FAkUGCDocumentValidator::Validate(Document);
    TestFalse(TEXT("Parent cycles are rejected"), Validation.IsValid());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCDocumentCrossSceneParentTest,
    "AkUGC.Core.Document.RejectsCrossSceneParents",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCDocumentCrossSceneParentTest::RunTest(const FString& Parameters)
{
    FAkUGCProjectDocument Document;
    Document.Manifest.ProjectId = FGuid::NewGuid();
    Document.Manifest.DisplayName = TEXT("Cross Scene Parent Test");
    Document.Manifest.TemplateId = TEXT("official.tower_defense");

    FAkUGCSceneDocument& FirstScene = Document.Scenes.AddDefaulted_GetRef();
    FirstScene.SceneId = FGuid::NewGuid();
    FAkUGCEntityRecord Parent;
    Parent.EntityId = FGuid::NewGuid();
    Parent.PrefabId = TEXT("official.gameplay.base");
    FirstScene.Entities.Add(Parent);

    FAkUGCSceneDocument& SecondScene = Document.Scenes.AddDefaulted_GetRef();
    SecondScene.SceneId = FGuid::NewGuid();
    FAkUGCEntityRecord Child;
    Child.EntityId = FGuid::NewGuid();
    Child.PrefabId = TEXT("official.gameplay.tower");
    Child.ParentEntityId = Parent.EntityId;
    SecondScene.Entities.Add(Child);

    const FAkUGCValidationResult Validation = FAkUGCDocumentValidator::Validate(Document);
    TestFalse(TEXT("Cross-scene parent references are rejected"), Validation.IsValid());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCDocumentLegacyMigrationTest,
    "AkUGC.Core.Document.Migration.LegacyV0ToCurrent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCDocumentLegacyMigrationTest::RunTest(const FString& Parameters)
{
    FAkUGCProjectDocument Source;
    Source.Manifest.ProjectId = FGuid::NewGuid();
    Source.Manifest.DisplayName = TEXT("Legacy Project");
    Source.Manifest.TemplateId = TEXT("official.tower_defense");
    FAkUGCSceneDocument& Scene = Source.Scenes.AddDefaulted_GetRef();
    Scene.SceneId = FGuid::NewGuid();
    Scene.DisplayName = TEXT("Main");
    FAkUGCEntityRecord& Entity = Scene.Entities.AddDefaulted_GetRef();
    Entity.EntityId = FGuid::NewGuid();
    Entity.PrefabId = TEXT("official.gameplay.base");
    Entity.Transform.SetLocation(FVector(12.0, 34.0, 56.0));
    FAkUGCComponentRecord& Component = Entity.Components.AddDefaulted_GetRef();
    Component.TypeId = TEXT("core.health");

    FString CurrentJson;
    FString Error;
    TestTrue(TEXT("Current source serializes"), FAkUGCDocumentJson::Serialize(Source, CurrentJson, &Error));

    TSharedPtr<FJsonObject> RootObject;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(CurrentJson);
    TestTrue(TEXT("Current JSON parses for legacy fixture"), FJsonSerializer::Deserialize(Reader, RootObject));
    if (!RootObject.IsValid())
    {
        return false;
    }

    const TSharedPtr<FJsonObject>* Manifest = nullptr;
    TestTrue(TEXT("Legacy fixture contains manifest"), RootObject->TryGetObjectField(TEXT("manifest"), Manifest));
    if (!Manifest || !Manifest->IsValid())
    {
        return false;
    }
    (*Manifest)->RemoveField(TEXT("schemaVersion"));

    const TArray<TSharedPtr<FJsonValue>>* Scenes = nullptr;
    if (!RootObject->TryGetArrayField(TEXT("scenes"), Scenes)
        || !Scenes
        || Scenes->IsEmpty()
        || (*Scenes)[0]->Type != EJson::Object)
    {
        AddError(TEXT("Legacy fixture scenes are invalid."));
        return false;
    }
    (*Scenes)[0]->AsObject()->RemoveField(TEXT("logicGraph"));
    (*Scenes)[0]->AsObject()->RemoveField(TEXT("ruleset"));
    const TArray<TSharedPtr<FJsonValue>>& Entities = (*Scenes)[0]->AsObject()->GetArrayField(TEXT("entities"));
    if (Entities.IsEmpty() || Entities[0]->Type != EJson::Object)
    {
        AddError(TEXT("Legacy fixture entities are invalid."));
        return false;
    }
    const TArray<TSharedPtr<FJsonValue>>& Components = Entities[0]->AsObject()->GetArrayField(TEXT("components"));
    if (Components.IsEmpty() || Components[0]->Type != EJson::Object)
    {
        AddError(TEXT("Legacy fixture components are invalid."));
        return false;
    }
    TSharedPtr<FJsonObject> LegacyComponent = Components[0]->AsObject();
    LegacyComponent->RemoveField(TEXT("schemaVersion"));

    FString LegacyJson;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&LegacyJson);
    TestTrue(TEXT("Legacy fixture serializes"), FJsonSerializer::Serialize(RootObject.ToSharedRef(), Writer));

    FAkUGCProjectDocument Migrated;
    FAkUGCDocumentMigrationResult Migration;
    if (!FAkUGCDocumentJson::Deserialize(LegacyJson, Migrated, &Error, &Migration))
    {
        AddError(FString::Printf(TEXT("Legacy V0 migration failed: %s"), *Error));
        return false;
    }
    TestTrue(TEXT("Migration succeeds"), Migration.bSucceeded);
    TestEqual(TEXT("Missing version is recognized as V0"), Migration.SourceVersion, 0);
    TestEqual(TEXT("Migration targets current version"), Migration.TargetVersion, AkUGCSchema::CurrentProjectDocumentVersion);
    TestEqual(TEXT("V0 to current applies five migration steps"), Migration.AppliedSteps.Num(), 5);
    if (Migrated.Scenes.IsEmpty()
        || Migrated.Scenes[0].Entities.IsEmpty()
        || Migrated.Scenes[0].Entities[0].Components.IsEmpty())
    {
        AddError(TEXT("Migrated document does not contain the expected component."));
        return false;
    }
    TestEqual(TEXT("Migrated manifest uses current version"), Migrated.Manifest.SchemaVersion, AkUGCSchema::CurrentProjectDocumentVersion);
    TestEqual(TEXT("Missing component version normalizes to V1"), Migrated.Scenes[0].Entities[0].Components[0].SchemaVersion, 1);
    TestTrue(TEXT("Logic Graph migration initializes an empty graph"), Migrated.Scenes[0].LogicGraph.Nodes.IsEmpty());
    TestTrue(TEXT("Ruleset migration initializes empty editable waves"), Migrated.Scenes[0].Ruleset.Waves.IsEmpty());
    TestEqual(TEXT("Ruleset migration initializes wave interval"), Migrated.Scenes[0].Ruleset.WaveIntervalSeconds, 5.0);
    TestEqual(TEXT("Project ID is preserved"), Migrated.Manifest.ProjectId, Source.Manifest.ProjectId);
    TestEqual(TEXT("Entity ID is preserved"), Migrated.Scenes[0].Entities[0].EntityId, Entity.EntityId);
    TestEqual(TEXT("Entity transform is preserved"), Migrated.Scenes[0].Entities[0].Transform.GetLocation(), Entity.Transform.GetLocation());
    TestTrue(TEXT("Migrated document validates"), FAkUGCDocumentValidator::Validate(Migrated).IsValid());

    FString SavedJson;
    TestTrue(TEXT("Migrated document saves as current version"), FAkUGCDocumentJson::Serialize(Migrated, SavedJson, &Error));
    TSharedPtr<FJsonObject> SavedRoot;
    const TSharedRef<TJsonReader<>> SavedReader = TJsonReaderFactory<>::Create(SavedJson);
    TestTrue(TEXT("Saved migrated JSON parses"), FJsonSerializer::Deserialize(SavedReader, SavedRoot));
    if (!SavedRoot.IsValid())
    {
        return false;
    }
    const TSharedPtr<FJsonObject>* SavedManifest = nullptr;
    if (!SavedRoot->TryGetObjectField(TEXT("manifest"), SavedManifest) || !SavedManifest || !SavedManifest->IsValid())
    {
        AddError(TEXT("Saved migrated JSON manifest is invalid."));
        return false;
    }
    TestEqual(
        TEXT("Saved migrated JSON uses current version"),
        (*SavedManifest)->GetIntegerField(TEXT("schemaVersion")),
        AkUGCSchema::CurrentProjectDocumentVersion);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCDocumentMigrationRejectionTest,
    "AkUGC.Core.Document.Migration.RejectsUnsupportedVersions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCDocumentMigrationRejectionTest::RunTest(const FString& Parameters)
{
    FAkUGCProjectDocument Output;
    Output.Manifest.ProjectId = FGuid::NewGuid();
    FString Error;
    FAkUGCDocumentMigrationResult Migration;

    const FString FutureJson = TEXT("{\"manifest\":{\"schemaVersion\":6},\"scenes\":[]}");
    TestFalse(TEXT("Future project version is rejected"), FAkUGCDocumentJson::Deserialize(FutureJson, Output, &Error, &Migration));
    TestEqual(TEXT("Future version is reported"), Migration.SourceVersion, 6);
    TestEqual(TEXT("Future version error path is precise"), Migration.ErrorPath, FString(TEXT("manifest.schemaVersion")));
    TestFalse(TEXT("Rejected output is reset instead of partially populated"), Output.Manifest.ProjectId.IsValid());

    const FString NegativeJson = TEXT("{\"manifest\":{\"schemaVersion\":-1},\"scenes\":[]}");
    TestFalse(TEXT("Negative project version is rejected"), FAkUGCDocumentJson::Deserialize(NegativeJson, Output, &Error, &Migration));
    TestEqual(TEXT("Negative version is reported"), Migration.SourceVersion, -1);

    const FString InvalidTypeJson = TEXT("{\"manifest\":{\"schemaVersion\":\"one\"},\"scenes\":[]}");
    TestFalse(TEXT("Non-numeric project version is rejected"), FAkUGCDocumentJson::Deserialize(InvalidTypeJson, Output, &Error, &Migration));
    TestEqual(TEXT("Invalid version error path is precise"), Migration.ErrorPath, FString(TEXT("manifest.schemaVersion")));

    const FString AliasJson = TEXT("{\"manifest\":{\"SchemaVersion\":2},\"scenes\":[]}");
    TestFalse(TEXT("Non-canonical version alias is rejected"), FAkUGCDocumentJson::Deserialize(AliasJson, Output, &Error, &Migration));
    TestEqual(TEXT("Version alias error path is precise"), Migration.ErrorPath, FString(TEXT("manifest.schemaVersion")));

    const FString ConflictingJson = TEXT("{\"manifest\":{\"schemaVersion\":1,\"SchemaVersion\":2},\"scenes\":[]}");
    TestFalse(TEXT("Conflicting version aliases are rejected"), FAkUGCDocumentJson::Deserialize(ConflictingJson, Output, &Error, &Migration));

    const FString RulesetAliasJson = TEXT("{\"manifest\":{\"schemaVersion\":3},\"scenes\":[{\"Ruleset\":{}}]}");
    TestFalse(TEXT("Non-canonical Ruleset alias is rejected"),
        FAkUGCDocumentJson::Deserialize(RulesetAliasJson, Output, &Error, &Migration));
    TestEqual(TEXT("Ruleset alias error path is precise"), Migration.ErrorPath, FString(TEXT("scenes[0].ruleset")));

    const FString DuplicateFieldJson = TEXT("{\"manifest\":{\"schemaVersion\":5,\"schemaVersion\":4},\"scenes\":[]}");
    TestFalse(TEXT("Exact duplicate JSON fields are rejected before overwrite"),
        FAkUGCDocumentJson::Deserialize(DuplicateFieldJson, Output, &Error, &Migration));
    TestTrue(TEXT("Duplicate field error is explicit"), Error.Contains(TEXT("duplicated")));

    const FString DuplicateEmptyFieldJson = TEXT("{\"manifest\":{\"schemaVersion\":4},\"scenes\":[],\"\":1,\"\":2}");
    TestFalse(TEXT("Duplicate empty JSON field names are rejected"),
        FAkUGCDocumentJson::Deserialize(DuplicateEmptyFieldJson, Output, &Error, &Migration));

    const FString V4NestedAliasJson = TEXT(
        "{\"manifest\":{\"schemaVersion\":4},\"scenes\":[{\"ruleset\":{"
        "\"Waves\":[],\"waveIntervalSeconds\":5,"
        "\"defeatCondition\":\"BaseHealthDepleted\","
        "\"victoryCondition\":\"AllWavesCleared\"}}]}");
    TestFalse(TEXT("V4 nested Ruleset aliases are rejected"),
        FAkUGCDocumentJson::Deserialize(V4NestedAliasJson, Output, &Error, &Migration));
    TestTrue(TEXT("Nested alias error identifies canonical field"), Error.Contains(TEXT("waves")));

    const FString V4NumericEnumJson = TEXT(
        "{\"manifest\":{\"schemaVersion\":4},\"scenes\":[{\"ruleset\":{"
        "\"waves\":[],\"waveIntervalSeconds\":5,\"defeatCondition\":0,"
        "\"victoryCondition\":\"AllWavesCleared\"}}]}");
    TestFalse(TEXT("V4 numeric Ruleset enums are rejected"),
        FAkUGCDocumentJson::Deserialize(V4NumericEnumJson, Output, &Error, &Migration));

    TSharedRef<FJsonObject> AtomicRoot = MakeShared<FJsonObject>();
    TSharedRef<FJsonObject> AtomicManifest = MakeShared<FJsonObject>();
    AtomicRoot->SetObjectField(TEXT("manifest"), AtomicManifest);
    TSharedRef<FJsonObject> AtomicScene = MakeShared<FJsonObject>();
    TSharedRef<FJsonObject> AtomicEntity = MakeShared<FJsonObject>();
    TSharedRef<FJsonObject> FirstComponent = MakeShared<FJsonObject>();
    TSharedRef<FJsonObject> InvalidComponent = MakeShared<FJsonObject>();
    InvalidComponent->SetStringField(TEXT("schemaVersion"), TEXT("invalid"));
    AtomicEntity->SetArrayField(TEXT("components"), {
        MakeShared<FJsonValueObject>(FirstComponent),
        MakeShared<FJsonValueObject>(InvalidComponent)});
    AtomicScene->SetArrayField(TEXT("entities"), {MakeShared<FJsonValueObject>(AtomicEntity)});
    AtomicRoot->SetArrayField(TEXT("scenes"), {MakeShared<FJsonValueObject>(AtomicScene)});
    const FAkUGCDocumentMigrationResult AtomicMigration = FAkUGCDocumentMigrator::Migrate(AtomicRoot);
    TestFalse(TEXT("Invalid later component rejects migration"), AtomicMigration.bSucceeded);
    TestFalse(TEXT("Failed migration does not modify earlier component"), FirstComponent->HasField(TEXT("schemaVersion")));
    TestFalse(TEXT("Failed migration does not modify manifest"), AtomicManifest->HasField(TEXT("schemaVersion")));

    FAkUGCProjectDocument OldDocument;
    OldDocument.Manifest.SchemaVersion = 0;
    FString Serialized;
    TestFalse(TEXT("Serializer rejects non-current document"), FAkUGCDocumentJson::Serialize(OldDocument, Serialized, &Error));
    TestTrue(TEXT("Rejected serialization remains empty"), Serialized.IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCDocumentInt64JsonTest,
    "AkUGC.Core.Document.Int64JsonSafety",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCDocumentInt64JsonTest::RunTest(const FString& Parameters)
{
    FAkUGCProjectDocument Source;
    Source.Manifest.ProjectId = FGuid::NewGuid();
    Source.Manifest.DisplayName = TEXT("Int64 Test");
    Source.Manifest.TemplateId = TEXT("official.tower_defense");
    FAkUGCSceneDocument& Scene = Source.Scenes.AddDefaulted_GetRef();
    Scene.SceneId = FGuid::NewGuid();
    FAkUGCEntityRecord& Entity = Scene.Entities.AddDefaulted_GetRef();
    Entity.EntityId = FGuid::NewGuid();
    Entity.PrefabId = TEXT("official.gameplay.base");
    FAkUGCComponentRecord& Component = Entity.Components.AddDefaulted_GetRef();
    Component.TypeId = TEXT("test.integer");
    FAkUGCValue IntegerValue;
    IntegerValue.Type = EAkUGCValueType::Integer;
    IntegerValue.IntegerValue = MAX_int64;
    Component.Properties.Add(TEXT("value"), IntegerValue);

    FString Json;
    FString Error;
    TestTrue(TEXT("Document with MAX_int64 serializes"), FAkUGCDocumentJson::Serialize(Source, Json, &Error));
    TestTrue(TEXT("MAX_int64 is encoded as a decimal string"), Json.Contains(TEXT("\"9223372036854775807\"")));

    FAkUGCProjectDocument Restored;
    if (!FAkUGCDocumentJson::Deserialize(Json, Restored, &Error))
    {
        AddError(FString::Printf(TEXT("MAX_int64 document failed to deserialize: %s"), *Error));
        return false;
    }
    if (Restored.Scenes.IsEmpty()
        || Restored.Scenes[0].Entities.IsEmpty()
        || Restored.Scenes[0].Entities[0].Components.IsEmpty())
    {
        AddError(TEXT("Restored int64 document does not contain the expected component."));
        return false;
    }
    const FAkUGCValue* RestoredValue = Restored.Scenes[0].Entities[0].Components[0].Properties.Find(TEXT("value"));
    TestNotNull(TEXT("Restored integer property exists"), RestoredValue);
    if (RestoredValue)
    {
        TestEqual(TEXT("MAX_int64 round-trips exactly"), RestoredValue->IntegerValue, MAX_int64);
    }

    const FString UnsafeNumericJson = Json.Replace(
        TEXT("\"9223372036854775807\""),
        TEXT("9007199254740992"));
    TestFalse(
        TEXT("Unsafe numeric int64 representation is rejected"),
        FAkUGCDocumentJson::Deserialize(UnsafeNumericJson, Restored, &Error));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCTowerDefenseRulesetValidationTest,
    "AkUGC.Core.Document.Ruleset.Validation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCTowerDefenseRulesetValidationTest::RunTest(const FString& Parameters)
{
    FAkUGCProjectDocument Document;
    Document.Manifest.ProjectId = FGuid::NewGuid();
    Document.Manifest.DisplayName = TEXT("Ruleset Validation");
    Document.Manifest.TemplateId = TEXT("official.tower_defense");
    FAkUGCSceneDocument& Scene = Document.Scenes.AddDefaulted_GetRef();
    Scene.SceneId = FGuid::NewGuid();

    TestTrue(TEXT("Empty Ruleset is a valid editing intermediate state"),
        FAkUGCDocumentValidator::Validate(Document).IsValid());

    for (int32 WaveIndex = 0; WaveIndex < AkUGCTowerDefenseRulesetLimits::RequiredWaveCount; ++WaveIndex)
    {
        FAkUGCEntityRecord& SpawnPoint = Scene.Entities.AddDefaulted_GetRef();
        SpawnPoint.EntityId = FGuid::NewGuid();
        SpawnPoint.PrefabId = TEXT("official.gameplay.enemy_spawn");
        FAkUGCTowerDefenseWave& Wave = Scene.Ruleset.Waves.AddDefaulted_GetRef();
        Wave.WaveId = FGuid::NewGuid();
        Wave.SpawnPointEntityId = SpawnPoint.EntityId;
        Wave.StartDelaySeconds = WaveIndex;
    }
    TestTrue(TEXT("Complete three-wave Ruleset validates"),
        FAkUGCDocumentValidator::Validate(Document).IsValid());

    const FGuid ThirdWaveId = Scene.Ruleset.Waves[2].WaveId;
    Scene.Ruleset.Waves[2].WaveId = Scene.Ruleset.Waves[0].WaveId;
    TestFalse(TEXT("Ruleset rejects duplicate Wave IDs"),
        FAkUGCDocumentValidator::Validate(Document).IsValid());
    Scene.Ruleset.Waves[2].WaveId = ThirdWaveId;

    FAkUGCTowerDefenseWave FourthWave = Scene.Ruleset.Waves[0];
    Scene.Ruleset.Waves.Add(FourthWave);
    TestFalse(TEXT("Ruleset rejects more than three waves"),
        FAkUGCDocumentValidator::Validate(Document).IsValid());
    Scene.Ruleset.Waves.Pop();

    Scene.Ruleset.Waves[0].StartDelaySeconds = AkUGCTowerDefenseRulesetLimits::MaxStartDelaySeconds + 1.0;
    TestFalse(TEXT("Ruleset rejects excessive Wave delay"),
        FAkUGCDocumentValidator::Validate(Document).IsValid());
    Scene.Ruleset.Waves[0].StartDelaySeconds = 0.0;

    Scene.Entities[0].PrefabId = TEXT("official.gameplay.base");
    TestFalse(TEXT("Wave must reference an official Enemy Spawn"),
        FAkUGCDocumentValidator::Validate(Document).IsValid());
    Scene.Entities[0].PrefabId = TEXT("official.gameplay.enemy_spawn");

    Scene.Ruleset.Waves[0].SpawnPointEntityId = FGuid::NewGuid();
    TestFalse(TEXT("Wave rejects missing cross-reference"),
        FAkUGCDocumentValidator::Validate(Document).IsValid());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCDocumentLogicLayoutMigrationTest,
    "AkUGC.Core.Document.Migration.InitializesLogicNodeLayout",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCDocumentLogicLayoutMigrationTest::RunTest(const FString& Parameters)
{
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    TSharedRef<FJsonObject> Manifest = MakeShared<FJsonObject>();
    Manifest->SetNumberField(TEXT("schemaVersion"), 4);
    Root->SetObjectField(TEXT("manifest"), Manifest);

    TSharedRef<FJsonObject> LogicGraph = MakeShared<FJsonObject>();
    TSharedRef<FJsonObject> StartNode = MakeShared<FJsonObject>();
    StartNode->SetStringField(TEXT("nodeId"), TEXT("11111111-1111-1111-1111-111111111111"));
    StartNode->SetStringField(TEXT("type"), TEXT("GameStart"));
    TSharedRef<FJsonObject> MessageNode = MakeShared<FJsonObject>();
    MessageNode->SetStringField(TEXT("nodeId"), TEXT("22222222-2222-2222-2222-222222222222"));
    MessageNode->SetStringField(TEXT("type"), TEXT("Message"));
    MessageNode->SetStringField(TEXT("message"), TEXT("Hello"));
    LogicGraph->SetArrayField(TEXT("nodes"), {
        MakeShared<FJsonValueObject>(StartNode),
        MakeShared<FJsonValueObject>(MessageNode)});
    LogicGraph->SetArrayField(TEXT("connections"), {});

    TSharedRef<FJsonObject> Scene = MakeShared<FJsonObject>();
    Scene->SetObjectField(TEXT("logicGraph"), LogicGraph);
    Scene->SetObjectField(TEXT("ruleset"), MakeShared<FJsonObject>());
    Root->SetArrayField(TEXT("scenes"), {MakeShared<FJsonValueObject>(Scene)});

    const FAkUGCDocumentMigrationResult Migration = FAkUGCDocumentMigrator::Migrate(Root);
    TestTrue(TEXT("V4 layout migration succeeds"), Migration.bSucceeded);
    TestTrue(TEXT("Layout migration step is recorded"), Migration.AppliedSteps.Contains(TEXT("ProjectDocumentV4ToV5")));
    if (!Migration.bSucceeded)
    {
        return false;
    }

    const TSharedPtr<FJsonObject>* MigratedManifest = nullptr;
    TestTrue(TEXT("Manifest survives migration"), Root->TryGetObjectField(TEXT("manifest"), MigratedManifest));
    TestEqual(
        TEXT("Manifest version advances to current"),
        static_cast<int32>((*MigratedManifest)->GetNumberField(TEXT("schemaVersion"))),
        AkUGCSchema::CurrentProjectDocumentVersion);

    const TArray<TSharedPtr<FJsonValue>>* Scenes = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
    if (!Root->TryGetArrayField(TEXT("scenes"), Scenes)
        || !Scenes
        || Scenes->IsEmpty()
        || (*Scenes)[0]->Type != EJson::Object)
    {
        AddError(TEXT("Migrated scenes are invalid."));
        return false;
    }
    const TSharedPtr<FJsonObject> MigratedScene = (*Scenes)[0]->AsObject();
    const TSharedPtr<FJsonObject>* MigratedGraph = nullptr;
    if (!MigratedScene->TryGetObjectField(TEXT("logicGraph"), MigratedGraph)
        || !MigratedGraph
        || !(*MigratedGraph)->TryGetArrayField(TEXT("nodes"), Nodes)
        || !Nodes
        || Nodes->Num() != 2)
    {
        AddError(TEXT("Migrated logic graph nodes are invalid."));
        return false;
    }

    const TSharedPtr<FJsonObject> FirstNode = (*Nodes)[0]->AsObject();
    const TSharedPtr<FJsonObject> SecondNode = (*Nodes)[1]->AsObject();
    TestEqual(TEXT("First node X is initialized"), FirstNode->GetNumberField(TEXT("positionX")), 0.0);
    TestEqual(TEXT("First node Y is initialized"), FirstNode->GetNumberField(TEXT("positionY")), 0.0);
    TestEqual(TEXT("Second node X is initialized"), SecondNode->GetNumberField(TEXT("positionX")), 0.0);
    TestEqual(TEXT("Second node Y is staggered below the first"), SecondNode->GetNumberField(TEXT("positionY")), 180.0);
    return true;
}

#endif

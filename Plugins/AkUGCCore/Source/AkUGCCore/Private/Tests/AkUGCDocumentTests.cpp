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
    TestEqual(TEXT("Entity ID round-trips"), Restored.Scenes[0].Entities[0].EntityId, Entity.EntityId);
    TestEqual(TEXT("Prefab ID round-trips"), Restored.Scenes[0].Entities[0].PrefabId, Entity.PrefabId);
    TestEqual(TEXT("Component count round-trips"), Restored.Scenes[0].Entities[0].Components.Num(), 1);

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
    TestEqual(TEXT("V0 to current applies two migration steps"), Migration.AppliedSteps.Num(), 2);
    if (Migrated.Scenes.IsEmpty()
        || Migrated.Scenes[0].Entities.IsEmpty()
        || Migrated.Scenes[0].Entities[0].Components.IsEmpty())
    {
        AddError(TEXT("Migrated document does not contain the expected component."));
        return false;
    }
    TestEqual(TEXT("Migrated manifest uses current version"), Migrated.Manifest.SchemaVersion, AkUGCSchema::CurrentProjectDocumentVersion);
    TestEqual(TEXT("Missing component version normalizes to V1"), Migrated.Scenes[0].Entities[0].Components[0].SchemaVersion, 1);
    TestTrue(TEXT("V2 migration initializes an empty Logic Graph"), Migrated.Scenes[0].LogicGraph.Nodes.IsEmpty());
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

    const FString FutureJson = TEXT("{\"manifest\":{\"schemaVersion\":3},\"scenes\":[]}");
    TestFalse(TEXT("Future project version is rejected"), FAkUGCDocumentJson::Deserialize(FutureJson, Output, &Error, &Migration));
    TestEqual(TEXT("Future version is reported"), Migration.SourceVersion, 3);
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

#endif

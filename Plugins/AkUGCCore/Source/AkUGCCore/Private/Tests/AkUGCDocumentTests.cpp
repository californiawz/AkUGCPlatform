#include "Misc/AutomationTest.h"

#include "Document/AkUGCDocument.h"
#include "Document/AkUGCDocumentJson.h"
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
    TestTrue(TEXT("Document deserializes"), FAkUGCDocumentJson::Deserialize(Json, Restored, &Error));
    TestEqual(TEXT("Project ID round-trips"), Restored.Manifest.ProjectId, Source.Manifest.ProjectId);
    TestEqual(TEXT("Scene count round-trips"), Restored.Scenes.Num(), 1);
    TestEqual(TEXT("Entity ID round-trips"), Restored.Scenes[0].Entities[0].EntityId, Entity.EntityId);
    TestEqual(TEXT("Prefab ID round-trips"), Restored.Scenes[0].Entities[0].PrefabId, Entity.PrefabId);
    TestEqual(TEXT("Component count round-trips"), Restored.Scenes[0].Entities[0].Components.Num(), 1);

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

#endif

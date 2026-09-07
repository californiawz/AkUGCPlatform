#include "Misc/AutomationTest.h"

#include "Document/AkUGCDocumentJson.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Subsystem/AkUGCAppEditorSubsystem.h"
#include "Subsystem/AkUGCLogicRuntimeSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCAppEditorSubsystemWorkflowTest,
    "AkUGC.Runtime.AppEditor.BlueprintWorkflow",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCAppEditorSubsystemWorkflowTest::RunTest(const FString& Parameters)
{
    if (!GEngine)
    {
        AddError(TEXT("Engine is not available."));
        return false;
    }

    UWorld* World = NewObject<UWorld>(GetTransientPackage(), NAME_None, RF_Transient);
    World->WorldType = EWorldType::Game;
    FWorldContext& WorldContext = GEngine->CreateNewWorldContext(World->WorldType);
    WorldContext.SetCurrentWorld(World);
    World->InitializeNewWorld(UWorld::InitializationValues()
        .InitializeScenes(false)
        .AllowAudioPlayback(false)
        .RequiresHitProxies(false)
        .CreatePhysicsScene(false)
        .CreateNavigation(false)
        .CreateAISystem(false)
        .ShouldSimulatePhysics(false)
        .EnableTraceCollision(false)
        .SetTransactional(false));

    UAkUGCAppEditorSubsystem* Subsystem = World->GetSubsystem<UAkUGCAppEditorSubsystem>();
    TestNotNull(TEXT("App Editor world subsystem exists"), Subsystem);
    if (!Subsystem)
    {
        GEngine->DestroyWorldContext(World);
        World->DestroyWorld(false);
        return false;
    }

    TestTrue(TEXT("App creates tower defense project"), Subsystem->NewTowerDefenseProject().bSucceeded);
    TestTrue(TEXT("App project is open"), Subsystem->HasOpenProject());
    TestEqual(TEXT("App exposes official prefabs"), Subsystem->GetAvailablePrefabIds().Num(), 11);

    FAkUGCProjectDocument LogicDocument = Subsystem->GetDocument();
    FAkUGCLogicNode GameStart;
    GameStart.NodeId = FGuid::NewGuid();
    GameStart.Type = EAkUGCLogicNodeType::GameStart;
    FAkUGCLogicNode Message;
    Message.NodeId = FGuid::NewGuid();
    Message.Type = EAkUGCLogicNodeType::Message;
    Message.Message = TEXT("Must not run while editing");
    FAkUGCLogicConnection LogicConnection;
    LogicConnection.SourceNodeId = GameStart.NodeId;
    LogicConnection.TargetNodeId = Message.NodeId;
    LogicDocument.Scenes[0].LogicGraph.Nodes = {GameStart, Message};
    LogicDocument.Scenes[0].LogicGraph.Connections = {LogicConnection};
    FString EditModeJson;
    FString EditModeError;
    TestTrue(TEXT("App edit-mode logic fixture serializes"),
        FAkUGCDocumentJson::Serialize(LogicDocument, EditModeJson, &EditModeError));
    UAkUGCLogicRuntimeSubsystem* LogicRuntime = World->GetSubsystem<UAkUGCLogicRuntimeSubsystem>();
    TestNotNull(TEXT("Logic Runtime subsystem exists beside App Editor"), LogicRuntime);
    TestTrue(TEXT("App loads project containing Game Start logic"), Subsystem->LoadProjectJson(EditModeJson).bSucceeded);
    if (LogicRuntime)
    {
        TestTrue(TEXT("App Edit session does not execute Game Start"), LogicRuntime->GetEmittedMessages().IsEmpty());
    }

    FGuid EnemyId;
    const FAkUGCAppEditResult PlaceResult = Subsystem->PlacePrefab(
        TEXT("official.unit.basic_enemy"),
        FTransform(FVector(100.0, 0.0, 0.0)),
        EnemyId);
    TestTrue(TEXT("App places an official prefab"), PlaceResult.bSucceeded);
    if (!PlaceResult.bSucceeded)
    {
        Subsystem->CloseProject();
        GEngine->DestroyWorldContext(World);
        World->DestroyWorld(false);
        return false;
    }

    FGuid InvalidScaleEntityId;
    TestFalse(TEXT("App rejects scaling for a fixed-scale prefab"), Subsystem->PlacePrefab(
        TEXT("official.unit.basic_enemy"),
        FTransform(FQuat::Identity, FVector::ZeroVector, FVector(2.0)),
        InvalidScaleEntityId).bSucceeded);
    TestFalse(TEXT("Rejected placement returns invalid EntityId"), InvalidScaleEntityId.IsValid());

    FAkUGCEntityRecord Enemy;
    TestTrue(TEXT("App reads placed entity"), Subsystem->GetEntityRecord(EnemyId, Enemy));
    TArray<FAkUGCPropertyDefinition> MobileProperties;
    TestTrue(TEXT("App reads mobile-editable schema"), Subsystem->GetEditableProperties(EnemyId, MobileProperties));
    TestFalse(TEXT("App schema hides desktop-only team ID"), MobileProperties.ContainsByPredicate([](const FAkUGCPropertyDefinition& Property)
    {
        return Property.PropertyId == TEXT("teamId");
    }));

    FAkUGCValue TeamId;
    TeamId.Type = EAkUGCValueType::Integer;
    TeamId.IntegerValue = 3;
    TestFalse(TEXT("App rejects desktop-only property edit"), Subsystem->SetEntityProperty(
        EnemyId, TEXT("core.team"), TEXT("teamId"), TeamId).bSucceeded);

    FAkUGCValue MaxHealth;
    MaxHealth.Type = EAkUGCValueType::Number;
    MaxHealth.NumberValue = 250.0;
    TestTrue(TEXT("App edits mobile property"), Subsystem->SetEntityProperty(
        EnemyId, TEXT("core.health"), TEXT("maxHealth"), MaxHealth).bSucceeded);
    TestTrue(TEXT("App undo is available"), Subsystem->CanUndo());
    TestTrue(TEXT("App undo succeeds"), Subsystem->Undo().bSucceeded);
    TestTrue(TEXT("App redo succeeds"), Subsystem->Redo().bSucceeded);

    FString Json;
    TestTrue(TEXT("App exports project JSON"), Subsystem->ExportProjectJson(Json).bSucceeded);
    TestFalse(TEXT("Exported project JSON is not empty"), Json.IsEmpty());

    const FString MaliciousJson = Json.Replace(
        TEXT("\"integerValue\": \"2\""),
        TEXT("\"integerValue\": \"3\""),
        ESearchCase::CaseSensitive);
    TestNotEqual(TEXT("Desktop-only property was modified in JSON fixture"), MaliciousJson, Json);
    TestFalse(TEXT("App rejects JSON that modifies a desktop-only property"), Subsystem->LoadProjectJson(MaliciousJson).bSucceeded);
    TestTrue(TEXT("Rejected JSON load preserves current project"), Subsystem->GetEntityRecord(EnemyId, Enemy));

    FAkUGCProjectDocument MissingReadonlyDocument;
    FString FixtureError;
    TestTrue(TEXT("Exported JSON parses for readonly deletion fixture"), FAkUGCDocumentJson::Deserialize(
        Json,
        MissingReadonlyDocument,
        &FixtureError));
    if (MissingReadonlyDocument.Scenes.IsEmpty()
        || MissingReadonlyDocument.Scenes[0].Entities.IsEmpty())
    {
        AddError(TEXT("Readonly deletion fixture does not contain the expected entity."));
        Subsystem->CloseProject();
        GEngine->DestroyWorldContext(World);
        World->DestroyWorld(false);
        return false;
    }
    FAkUGCEntityRecord& FixtureEntity = MissingReadonlyDocument.Scenes[0].Entities[0];
    FAkUGCComponentRecord* TeamComponent = FixtureEntity.Components.FindByPredicate([](const FAkUGCComponentRecord& Component)
    {
        return Component.TypeId == TEXT("core.team");
    });
    if (!TeamComponent)
    {
        AddError(TEXT("Readonly deletion fixture does not contain core.team."));
        Subsystem->CloseProject();
        GEngine->DestroyWorldContext(World);
        World->DestroyWorld(false);
        return false;
    }
    TeamComponent->Properties.Remove(TEXT("teamId"));
    FString MissingReadonlyJson;
    TestTrue(TEXT("Readonly deletion fixture serializes"), FAkUGCDocumentJson::Serialize(
        MissingReadonlyDocument,
        MissingReadonlyJson,
        &FixtureError));
    TestFalse(TEXT("App rejects JSON that removes a desktop-only property"), Subsystem->LoadProjectJson(MissingReadonlyJson).bSucceeded);

    FAkUGCProjectDocument OverBudgetDocument = Subsystem->GetDocument();
    const FAkUGCEntityRecord TemplateEntity = OverBudgetDocument.Scenes[0].Entities[0];
    while (OverBudgetDocument.Scenes[0].Entities.Num() <= 500)
    {
        FAkUGCEntityRecord& Extra = OverBudgetDocument.Scenes[0].Entities.Add_GetRef(TemplateEntity);
        Extra.EntityId = FGuid::NewGuid();
    }
    FString OverBudgetJson;
    TestTrue(TEXT("Over-budget fixture serializes"), FAkUGCDocumentJson::Serialize(
        OverBudgetDocument,
        OverBudgetJson,
        &FixtureError));
    TestFalse(TEXT("App rejects JSON beyond mobile content budget"), Subsystem->LoadProjectJson(OverBudgetJson).bSucceeded);

    Subsystem->CloseProject();
    TestFalse(TEXT("Close clears App project"), Subsystem->HasOpenProject());
    TestTrue(TEXT("App reloads exported JSON"), Subsystem->LoadProjectJson(Json).bSucceeded);
    TestTrue(TEXT("Reloaded App project restores entity"), Subsystem->GetEntityRecord(EnemyId, Enemy));

    Subsystem->CloseProject();
    GEngine->DestroyWorldContext(World);
    World->DestroyWorld(false);
    return true;
}

#endif

#include "Misc/AutomationTest.h"

#include "Containers/Ticker.h"
#include "Document/AkUGCDocumentJson.h"
#include "Editor.h"
#include "GameFramework/Actor.h"
#include "Misc/App.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Subsystem/AkUGCEditorSubsystem.h"
#include "Widgets/SAkUGCCreatorPanel.h"
#include "Widgets/SAkUGCWorkLibraryPanel.h"
#include "Pack/AkUGCWorkLibrary.h"
#include "Pack/AkUGCPublisherKeyStore.h"
#include "Pack/AkUGCLogicPackHasher.h"
#include "Template/AkUGCCopilotPreview.h"
#include "Widgets/SAkUGCCopilotPanel.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCEditorSubsystemWorkflowTest,
    "AkUGC.Editor.CreatorStudio.MinimalWorkflow",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCEditorSubsystemWorkflowTest::RunTest(const FString& Parameters)
{
    UAkUGCEditorSubsystem* Subsystem = GEditor
        ? GEditor->GetEditorSubsystem<UAkUGCEditorSubsystem>()
        : nullptr;
    TestNotNull(TEXT("Creator Studio subsystem exists"), Subsystem);
    if (!Subsystem)
    {
        return false;
    }

    FString Error;
    if (!Subsystem->NewTowerDefenseProject(&Error))
    {
        AddError(FString::Printf(TEXT("Failed to create project: %s"), *Error));
        return false;
    }
    const int32 TemplateEntityCount = Subsystem->GetDocument().Scenes[0].Entities.Num();
    TestTrue(TEXT("Default tower defense template is not empty"), TemplateEntityCount > 0);
    TestFalse(TEXT("New template starts with no edit history"), Subsystem->CanUndo());

    FGuid EntityId;
    const FAkUGCCommandExecutionResult PlaceResult = Subsystem->PlacePrefab(
        TEXT("official.gameplay.base"),
        FTransform(FVector(100.0, 0.0, 0.0)),
        EntityId);
    TestTrue(TEXT("Official prefab is placed"), PlaceResult.bSucceeded);
    TestEqual(TEXT("Document contains placed entity"), Subsystem->GetDocument().Scenes[0].Entities.Num(), TemplateEntityCount + 1);
    if (!IsRunningCommandlet() && !FApp::IsUnattended())
    {
        TestEqual(TEXT("Placed entity is selected in the editor"), Subsystem->GetSelectedEntityId(), EntityId);
    }

    FGuid DuplicateId;
    TestTrue(TEXT("Entity duplicates"), Subsystem->DuplicateEntity(EntityId, DuplicateId).bSucceeded);
    if (!IsRunningCommandlet() && !FApp::IsUnattended())
    {
        TestEqual(TEXT("Duplicate is selected"), Subsystem->GetSelectedEntityId(), DuplicateId);
    }
    TestEqual(TEXT("Document contains duplicate"), Subsystem->GetDocument().Scenes[0].Entities.Num(), TemplateEntityCount + 2);
    TestTrue(TEXT("Duplicate deletes"), Subsystem->DeleteEntity(DuplicateId).bSucceeded);
    TestEqual(TEXT("Delete removes duplicate"), Subsystem->GetDocument().Scenes[0].Entities.Num(), TemplateEntityCount + 1);
    TestTrue(TEXT("Undo delete restores duplicate"), Subsystem->Undo().bSucceeded);
    TestEqual(TEXT("Undo delete restores document entity"), Subsystem->GetDocument().Scenes[0].Entities.Num(), TemplateEntityCount + 2);
    TestTrue(TEXT("Undo duplicate removes duplicate"), Subsystem->Undo().bSucceeded);
    TestEqual(TEXT("Undo duplicate restores one entity"), Subsystem->GetDocument().Scenes[0].Entities.Num(), TemplateEntityCount + 1);

    TestTrue(TEXT("Creator undo succeeds"), Subsystem->Undo().bSucceeded);
    TestEqual(TEXT("Undo removes document entity"), Subsystem->GetDocument().Scenes[0].Entities.Num(), TemplateEntityCount);
    TestTrue(TEXT("Creator redo succeeds"), Subsystem->Redo().bSucceeded);
    TestEqual(TEXT("Redo restores document entity"), Subsystem->GetDocument().Scenes[0].Entities.Num(), TemplateEntityCount + 1);

    const FString TestPath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation/CreatorStudioWorkflow.json"));
    TestTrue(TEXT("Creator project saves"), Subsystem->SaveProject(TestPath, &Error));
    Subsystem->CloseProject();
    TestFalse(TEXT("Close clears active project"), Subsystem->HasOpenProject());
    TestTrue(TEXT("Creator project loads"), Subsystem->LoadProject(TestPath, &Error));
    TestEqual(TEXT("Loaded project restores entity"), Subsystem->GetDocument().Scenes[0].Entities.Num(), TemplateEntityCount + 1);

    Subsystem->CloseProject();
    IFileManager::Get().Delete(*TestPath, false, true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCEditorSchemaPropertyTest,
    "AkUGC.Editor.CreatorStudio.SchemaPropertyEditing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCEditorSchemaPropertyTest::RunTest(const FString& Parameters)
{
    UAkUGCEditorSubsystem* Subsystem = GEditor
        ? GEditor->GetEditorSubsystem<UAkUGCEditorSubsystem>()
        : nullptr;
    TestNotNull(TEXT("Creator Studio subsystem exists"), Subsystem);
    if (!Subsystem)
    {
        return false;
    }

    FString Error;
    TestTrue(TEXT("Tower defense project is created"), Subsystem->NewTowerDefenseProject(&Error));

    FGuid EntityId;
    TestTrue(
        TEXT("Base prefab is placed"),
        Subsystem->PlacePrefab(TEXT("official.gameplay.base"), FTransform::Identity, EntityId).bSucceeded);
    const uint64 RevisionBeforeEdit = Subsystem->GetDocumentRevision();

    FAkUGCValue MaxHealth;
    MaxHealth.Type = EAkUGCValueType::Number;
    MaxHealth.NumberValue = 2500.0;
    TestTrue(
        TEXT("Schema property edit succeeds"),
        Subsystem->SetEntityProperty(EntityId, TEXT("core.health"), TEXT("maxHealth"), MaxHealth).bSucceeded);
    TestTrue(TEXT("Property edit advances document revision"), Subsystem->GetDocumentRevision() > RevisionBeforeEdit);

    const FAkUGCEntityRecord* Entity = Subsystem->FindEntity(EntityId);
    TestNotNull(TEXT("Edited entity exists"), Entity);
    const FAkUGCComponentRecord* Health = Entity
        ? Entity->Components.FindByPredicate([](const FAkUGCComponentRecord& Component)
        {
            return Component.TypeId == TEXT("core.health");
        })
        : nullptr;
    TestNotNull(TEXT("Health component exists"), Health);
    const FAkUGCValue* StoredMaxHealth = Health ? Health->Properties.Find(TEXT("maxHealth")) : nullptr;
    TestNotNull(TEXT("Edited maxHealth exists"), StoredMaxHealth);
    if (StoredMaxHealth)
    {
        TestEqual(TEXT("Edited maxHealth is stored"), StoredMaxHealth->NumberValue, 2500.0);
    }

    const uint64 RevisionBeforeInvalidEdit = Subsystem->GetDocumentRevision();
    MaxHealth.NumberValue = 200000.0;
    TestFalse(
        TEXT("Out-of-range schema property is rejected"),
        Subsystem->SetEntityProperty(EntityId, TEXT("core.health"), TEXT("maxHealth"), MaxHealth).bSucceeded);
    MaxHealth.NumberValue = TNumericLimits<double>::Max() * 2.0;
    TestFalse(
        TEXT("Non-finite schema property is rejected"),
        Subsystem->SetEntityProperty(EntityId, TEXT("core.health"), TEXT("maxHealth"), MaxHealth).bSucceeded);
    TestEqual(
        TEXT("Rejected property edits do not advance document revision"),
        Subsystem->GetDocumentRevision(),
        RevisionBeforeInvalidEdit);

    TestTrue(TEXT("Undo property edit succeeds"), Subsystem->Undo().bSucceeded);
    Entity = Subsystem->FindEntity(EntityId);
    Health = Entity
        ? Entity->Components.FindByPredicate([](const FAkUGCComponentRecord& Component)
        {
            return Component.TypeId == TEXT("core.health");
        })
        : nullptr;
    StoredMaxHealth = Health ? Health->Properties.Find(TEXT("maxHealth")) : nullptr;
    TestNotNull(TEXT("Undo keeps maxHealth property"), StoredMaxHealth);
    if (StoredMaxHealth)
    {
        TestEqual(TEXT("Undo restores default maxHealth"), StoredMaxHealth->NumberValue, 1000.0);
    }

    TestTrue(TEXT("Redo property edit succeeds"), Subsystem->Redo().bSucceeded);
    Entity = Subsystem->FindEntity(EntityId);
    Health = Entity
        ? Entity->Components.FindByPredicate([](const FAkUGCComponentRecord& Component)
        {
            return Component.TypeId == TEXT("core.health");
        })
        : nullptr;
    StoredMaxHealth = Health ? Health->Properties.Find(TEXT("maxHealth")) : nullptr;
    TestNotNull(TEXT("Redo keeps maxHealth property"), StoredMaxHealth);
    if (StoredMaxHealth)
    {
        TestEqual(TEXT("Redo restores edited maxHealth"), StoredMaxHealth->NumberValue, 2500.0);
    }

    Subsystem->CloseProject();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCEditorViewportHierarchySyncTest,
    "AkUGC.Editor.CreatorStudio.ViewportHierarchySync",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCEditorViewportHierarchySyncTest::RunTest(const FString& Parameters)
{
    UAkUGCEditorSubsystem* Subsystem = GEditor
        ? GEditor->GetEditorSubsystem<UAkUGCEditorSubsystem>()
        : nullptr;
    TestNotNull(TEXT("Creator Studio subsystem exists"), Subsystem);
    if (!Subsystem)
    {
        return false;
    }

    FString Error;
    if (!Subsystem->NewTowerDefenseProject(&Error))
    {
        AddError(FString::Printf(TEXT("Failed to create project: %s"), *Error));
        return false;
    }

    FGuid ParentAId;
    FGuid ParentBId;
    FGuid ChildId;
    TestTrue(TEXT("First parent is placed"), Subsystem->PlacePrefab(
        TEXT("official.gameplay.base"), FTransform(FVector(100.0, 0.0, 0.0)), ParentAId).bSucceeded);
    TestTrue(TEXT("Second parent is placed"), Subsystem->PlacePrefab(
        TEXT("official.gameplay.base"), FTransform(FVector(500.0, 0.0, 0.0)), ParentBId).bSucceeded);
    TestTrue(TEXT("Child is placed"), Subsystem->PlacePrefab(
        TEXT("official.gameplay.tower_slot"), FTransform(FVector(250.0, 0.0, 0.0)), ChildId).bSucceeded);

    AActor* ParentA = Subsystem->FindRuntimeActor(ParentAId);
    AActor* ParentB = Subsystem->FindRuntimeActor(ParentBId);
    AActor* Child = Subsystem->FindRuntimeActor(ChildId);
    TestNotNull(TEXT("First parent actor exists"), ParentA);
    TestNotNull(TEXT("Second parent actor exists"), ParentB);
    TestNotNull(TEXT("Child actor exists"), Child);
    if (!ParentA || !ParentB || !Child)
    {
        Subsystem->CloseProject();
        return false;
    }

    const FVector OriginalChildLocation = Child->GetActorLocation();
    TestTrue(TEXT("Native editor attachment succeeds"), Child->AttachToActor(
        ParentA,
        FAttachmentTransformRules::KeepWorldTransform));
    const FAkUGCEntityRecord* ChildEntity = Subsystem->FindEntity(ChildId);
    TestNotNull(TEXT("Attached child remains in document"), ChildEntity);
    if (ChildEntity)
    {
        TestEqual(TEXT("Native attachment updates document parent"), ChildEntity->ParentEntityId, ParentAId);
        TestEqual(TEXT("Native attachment preserves document world transform"), ChildEntity->Transform.GetLocation(), OriginalChildLocation);
    }

    TestTrue(TEXT("Native editor reparent succeeds"), Child->AttachToActor(
        ParentB,
        FAttachmentTransformRules::KeepWorldTransform));
    ChildEntity = Subsystem->FindEntity(ChildId);
    if (ChildEntity)
    {
        TestEqual(TEXT("Native reparent updates document parent"), ChildEntity->ParentEntityId, ParentBId);
    }
    TestTrue(TEXT("Undo native reparent succeeds"), Subsystem->Undo().bSucceeded);
    TestEqual(TEXT("Undo restores first runtime parent"), Child->GetAttachParentActor(), ParentA);
    TestTrue(TEXT("Redo native reparent succeeds"), Subsystem->Redo().bSucceeded);
    TestEqual(TEXT("Redo restores second runtime parent"), Child->GetAttachParentActor(), ParentB);

    Child->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
    FTSTicker::GetCoreTicker().Tick(0.0f);
    ChildEntity = Subsystem->FindEntity(ChildId);
    if (ChildEntity)
    {
        TestFalse(TEXT("Native detach moves document entity to root"), ChildEntity->ParentEntityId.IsValid());
    }
    TestNull(TEXT("Native detach leaves runtime actor at root"), Child->GetAttachParentActor());
    TestEqual(TEXT("Native hierarchy edits preserve world transform"), Child->GetActorLocation(), OriginalChildLocation);

    Subsystem->CloseProject();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCEditorViewportDeletionSyncTest,
    "AkUGC.Editor.CreatorStudio.ViewportDeletionSync",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCEditorViewportDeletionSyncTest::RunTest(const FString& Parameters)
{
    UAkUGCEditorSubsystem* Subsystem = GEditor
        ? GEditor->GetEditorSubsystem<UAkUGCEditorSubsystem>()
        : nullptr;
    TestNotNull(TEXT("Creator Studio subsystem exists"), Subsystem);
    if (!Subsystem)
    {
        return false;
    }

    FString Error;
    if (!Subsystem->NewTowerDefenseProject(&Error))
    {
        AddError(FString::Printf(TEXT("Failed to create project: %s"), *Error));
        return false;
    }

    FGuid ParentId;
    FGuid ChildId;
    TestTrue(TEXT("Parent is placed"), Subsystem->PlacePrefab(
        TEXT("official.gameplay.base"), FTransform(FVector(100.0, 0.0, 0.0)), ParentId).bSucceeded);
    TestTrue(TEXT("Child is placed"), Subsystem->PlacePrefab(
        TEXT("official.gameplay.tower_slot"), FTransform(FVector(250.0, 0.0, 0.0)), ChildId).bSucceeded);
    TestTrue(TEXT("Child is parented"), Subsystem->SetEntityParent(ChildId, ParentId).bSucceeded);

    AActor* ParentActor = Subsystem->FindRuntimeActor(ParentId);
    AActor* ChildActor = Subsystem->FindRuntimeActor(ChildId);
    TestNotNull(TEXT("Parent runtime actor exists"), ParentActor);
    TestNotNull(TEXT("Child runtime actor exists"), ChildActor);
    if (!ParentActor || !ChildActor)
    {
        Subsystem->CloseProject();
        return false;
    }

    const FVector ChildWorldLocation = ChildActor->GetActorLocation();
    UWorld* World = ParentActor->GetWorld();
    TestNotNull(TEXT("Editor world exists"), World);
    if (!World)
    {
        Subsystem->CloseProject();
        return false;
    }

    FEditorDelegates::OnDeleteActorsBegin.Broadcast();
    TestTrue(TEXT("Native editor deletion succeeds"), World->EditorDestroyActor(ParentActor, true));
    FEditorDelegates::OnDeleteActorsEnd.Broadcast();
    TestNull(TEXT("Deleted parent is removed from document"), Subsystem->FindEntity(ParentId));
    TestNull(TEXT("Deleted parent is removed from runtime"), Subsystem->FindRuntimeActor(ParentId));

    const FAkUGCEntityRecord* ChildEntity = Subsystem->FindEntity(ChildId);
    TestNotNull(TEXT("Surviving child remains in document"), ChildEntity);
    if (ChildEntity)
    {
        TestFalse(TEXT("Surviving child moves to scene root"), ChildEntity->ParentEntityId.IsValid());
        TestEqual(TEXT("Surviving child keeps document world transform"), ChildEntity->Transform.GetLocation(), ChildWorldLocation);
    }
    TestNull(TEXT("Surviving runtime child moves to scene root"), ChildActor->GetAttachParentActor());

    TestTrue(TEXT("Undo native deletion succeeds"), Subsystem->Undo().bSucceeded);
    AActor* RestoredParent = Subsystem->FindRuntimeActor(ParentId);
    TestNotNull(TEXT("Undo restores parent runtime actor"), RestoredParent);
    TestEqual(TEXT("Undo restores child attachment"), ChildActor->GetAttachParentActor(), RestoredParent);
    TestTrue(TEXT("Redo native deletion succeeds"), Subsystem->Redo().bSucceeded);
    TestNull(TEXT("Redo removes parent runtime actor"), Subsystem->FindRuntimeActor(ParentId));
    TestNull(TEXT("Redo detaches surviving child"), ChildActor->GetAttachParentActor());

    Subsystem->CloseProject();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCEditorLogicListEditingTest,
    "AkUGC.Editor.CreatorStudio.LogicListEditing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCEditorLogicListEditingTest::RunTest(const FString& Parameters)
{
    UAkUGCEditorSubsystem* Subsystem = GEditor
        ? GEditor->GetEditorSubsystem<UAkUGCEditorSubsystem>()
        : nullptr;
    TestNotNull(TEXT("Creator Studio subsystem exists"), Subsystem);
    if (!Subsystem)
    {
        return false;
    }

    FString Error;
    if (!Subsystem->NewTowerDefenseProject(&Error))
    {
        AddError(FString::Printf(TEXT("Failed to create project: %s"), *Error));
        return false;
    }
    TArray<FGuid> InitialNodeIds;
    for (const FAkUGCLogicNode& Node : Subsystem->GetLogicGraph()->Nodes)
    {
        InitialNodeIds.Add(Node.NodeId);
    }
    for (const FGuid& NodeId : InitialNodeIds)
    {
        const FAkUGCCommandExecutionResult Result = Subsystem->DeleteLogicNode(NodeId);
        if (!Result.bSucceeded)
        {
            AddError(FString::Printf(TEXT("Failed to prepare isolated advanced Logic test graph: %s"), *Result.ErrorMessage));
            Subsystem->CloseProject();
            return false;
        }
    }
    const int32 TemplateNodeCount = Subsystem->GetLogicGraph()->Nodes.Num();
    const int32 TemplateConnectionCount = Subsystem->GetLogicGraph()->Connections.Num();

    FGuid AnchorId;
    TestTrue(
        TEXT("Spawn anchor entity is placed"),
        Subsystem->PlacePrefab(TEXT("official.gameplay.base"), FTransform::Identity, AnchorId).bSucceeded);

    const FGuid GameStartId(10, 0, 0, 0);
    const FGuid MessageId(11, 0, 0, 0);
    const FGuid SpawnId(12, 0, 0, 0);

    FAkUGCLogicNode GameStart;
    GameStart.NodeId = GameStartId;
    GameStart.Type = EAkUGCLogicNodeType::GameStart;
    TestTrue(TEXT("GameStart node is added"), Subsystem->AddLogicNode(GameStart).bSucceeded);

    FAkUGCLogicNode Message;
    Message.NodeId = MessageId;
    Message.Type = EAkUGCLogicNodeType::Message;
    Message.Message = TEXT("Wave incoming");
    TestTrue(TEXT("Message node is added"), Subsystem->AddLogicNode(Message).bSucceeded);
    TestTrue(TEXT("GameStart connects to message"), Subsystem->ConnectLogicNode(GameStartId, MessageId).bSucceeded);

    FAkUGCLogicNode Spawn;
    Spawn.NodeId = SpawnId;
    Spawn.Type = EAkUGCLogicNodeType::Spawn;
    Spawn.SpawnPrefabId = TEXT("official.unit.basic_enemy");
    Spawn.SpawnAtEntityId = AnchorId;
    TestTrue(TEXT("Spawn node is added"), Subsystem->AddLogicNode(Spawn).bSucceeded);
    TestTrue(TEXT("GameStart connects to spawn"), Subsystem->ConnectLogicNode(GameStartId, SpawnId).bSucceeded);

    {
        const FAkUGCLogicGraph* Graph = Subsystem->GetLogicGraph();
        TestNotNull(TEXT("Logic graph is exposed"), Graph);
        if (Graph)
        {
            TestEqual(TEXT("Logic graph lists three nodes"), Graph->Nodes.Num(), TemplateNodeCount + 3);
            TestEqual(TEXT("Logic graph lists two connections"), Graph->Connections.Num(), TemplateConnectionCount + 2);
        }
    }

    FAkUGCLogicNode UpdatedMessage = Message;
    UpdatedMessage.Message = TEXT("Final wave");
    TestTrue(TEXT("Message node updates"), Subsystem->UpdateLogicNode(UpdatedMessage).bSucceeded);
    {
        const FAkUGCLogicGraph* Graph = Subsystem->GetLogicGraph();
        const FAkUGCLogicNode* StoredMessage = Graph
            ? Graph->Nodes.FindByPredicate([&MessageId](const FAkUGCLogicNode& Node) { return Node.NodeId == MessageId; })
            : nullptr;
        TestNotNull(TEXT("Updated message node exists"), StoredMessage);
        if (StoredMessage)
        {
            TestEqual(TEXT("Message parameter is stored"), StoredMessage->Message, FString(TEXT("Final wave")));
        }
    }

    TestTrue(TEXT("Valid logic graph validates"), Subsystem->ValidateLogicGraph().IsValid());

    TestTrue(TEXT("Disconnect logic succeeds"), Subsystem->DisconnectLogicNode(GameStartId, SpawnId).bSucceeded);
    {
        const FAkUGCLogicGraph* Graph = Subsystem->GetLogicGraph();
        if (Graph)
        {
            TestEqual(TEXT("Disconnect leaves one connection"), Graph->Connections.Num(), TemplateConnectionCount + 1);
        }
    }
    TestTrue(TEXT("Undo disconnect succeeds"), Subsystem->Undo().bSucceeded);
    {
        const FAkUGCLogicGraph* Graph = Subsystem->GetLogicGraph();
        if (Graph)
        {
            TestEqual(TEXT("Undo restores two connections"), Graph->Connections.Num(), TemplateConnectionCount + 2);
        }
    }
    TestTrue(TEXT("Redo disconnect succeeds"), Subsystem->Redo().bSucceeded);
    {
        const FAkUGCLogicGraph* Graph = Subsystem->GetLogicGraph();
        if (Graph)
        {
            TestEqual(TEXT("Redo disconnect leaves one connection"), Graph->Connections.Num(), TemplateConnectionCount + 1);
        }
    }

    // 重新连接 GameStart -> Spawn，使 Spawn 在删除时仍拥有一条连接，
    // 从而真正覆盖「删除节点级联删除连接」与「撤销删除恢复连接」的路径。
    TestTrue(TEXT("Reconnect spawn for cascade test succeeds"), Subsystem->ConnectLogicNode(GameStartId, SpawnId).bSucceeded);

    TestTrue(TEXT("Spawn node deletes"), Subsystem->DeleteLogicNode(SpawnId).bSucceeded);
    {
        const FAkUGCLogicGraph* Graph = Subsystem->GetLogicGraph();
        if (Graph)
        {
            TestEqual(TEXT("Delete removes spawn node"), Graph->Nodes.Num(), TemplateNodeCount + 2);
            TestEqual(TEXT("Delete cascades spawn connection"), Graph->Connections.Num(), TemplateConnectionCount + 1);
        }
    }
    TestTrue(TEXT("Undo delete node succeeds"), Subsystem->Undo().bSucceeded);
    {
        const FAkUGCLogicGraph* Graph = Subsystem->GetLogicGraph();
        if (Graph)
        {
            TestEqual(TEXT("Undo restores three nodes"), Graph->Nodes.Num(), TemplateNodeCount + 3);
            TestEqual(TEXT("Undo restores spawn connection"), Graph->Connections.Num(), TemplateConnectionCount + 2);
        }
    }

    FAkUGCLogicGraph InvalidGraph;
    FAkUGCLogicNode EmptyMessage;
    EmptyMessage.NodeId = FGuid(20, 0, 0, 0);
    EmptyMessage.Type = EAkUGCLogicNodeType::Message;
    InvalidGraph.Nodes.Add(EmptyMessage);
    TestFalse(TEXT("Empty message graph is rejected"),
        FAkUGCDocumentValidator::ValidateLogicGraph(InvalidGraph).IsValid());

    Subsystem->CloseProject();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCEditorTowerDefenseParameterWorkflowTest,
    "AkUGC.Editor.CreatorStudio.TowerDefenseParameterWorkflow",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCEditorTowerDefenseParameterWorkflowTest::RunTest(const FString& Parameters)
{
    UAkUGCEditorSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UAkUGCEditorSubsystem>() : nullptr;
    TestNotNull(TEXT("Creator Studio subsystem exists"), Subsystem);
    if (!Subsystem)
    {
        return false;
    }
    FString Error;
    if (!Subsystem->NewTowerDefenseProject(&Error))
    {
        AddError(FString::Printf(TEXT("Failed to create default template: %s"), *Error));
        return false;
    }
    FAkUGCTowerDefenseParameters DefaultParameters;
    if (!Subsystem->GetTowerDefenseParameters(DefaultParameters, &Error))
    {
        AddError(FString::Printf(TEXT("Failed to read default template: %s"), *Error));
        Subsystem->CloseProject();
        return false;
    }
    TestEqual(TEXT("Default template exposes three waves"), DefaultParameters.Waves.Num(), 3);
    TestTrue(TEXT("Default template contains scene entities"), !Subsystem->GetDocument().Scenes[0].Entities.IsEmpty());
    TestFalse(TEXT("Default template has no Undo history"), Subsystem->CanUndo());
    for (const FAkUGCEntityRecord& Entity : Subsystem->GetDocument().Scenes[0].Entities)
    {
        TestNotNull(TEXT("Default template entity has an editor projection"), Subsystem->FindRuntimeActor(Entity.EntityId));
    }
    const auto Snapshot = [Subsystem]()
    {
        FString Json;
        FAkUGCDocumentJson::Serialize(Subsystem->GetDocument(), Json);
        return Json;
    };
    const FString OriginalDocument = Snapshot();
    const TSharedRef<SAkUGCTowerDefenseParameterForm> Form = SNew(SAkUGCTowerDefenseParameterForm, Subsystem);
    TestTrue(TEXT("Simple form reads current document on construction"), Form->HasReadableParameters());
    TestEqual(TEXT("Simple form initially displays three waves"), Form->GetDisplayedParameters().Waves.Num(), 3);
    FAkUGCTowerDefenseParameters Edited = DefaultParameters;
    Edited.Waves.SetNum(2);
    Edited.BaseMaxHealth = 1700.0;
    Edited.InitialResources = 425;
    Edited.WaveIntervalSeconds = 7.5;
    for (int32 Index = 0; Index < Edited.Waves.Num(); ++Index)
    {
        FAkUGCTowerDefenseWaveParameters& Wave = Edited.Waves[Index];
        Wave.EnemyPrefabId = TEXT("official.unit.basic_enemy");
        Wave.EnemyCount = 4 + Index;
        Wave.SpawnIntervalSeconds = 0.75 + Index;
        Wave.StartDelaySeconds = 1.25 + Index;
        Wave.EnemyMaxHealth = 120.0 + Index * 30.0;
        Wave.EnemyMoveSpeed = 280.0 + Index * 20.0;
        Wave.EnemyGoalDamage = 12.0 + Index;
    }
    const uint64 RevisionBeforeApply = Subsystem->GetDocumentRevision();
    TestTrue(TEXT("Parameter form applies through runtime command service"), Subsystem->ApplyTowerDefenseParameters(Edited, &Error));
    TestEqual(TEXT("One apply advances revision once"), Subsystem->GetDocumentRevision(), RevisionBeforeApply + 1);
    Form->Tick(FGeometry(), 0.0, 0.0f);
    TestEqual(TEXT("Simple form refreshes applied wave count"), Form->GetDisplayedParameters().Waves.Num(), 2);
    TestEqual(TEXT("Simple form refreshes applied base life"), Form->GetDisplayedParameters().BaseMaxHealth, Edited.BaseMaxHealth);
    FAkUGCTowerDefenseParameters ReadBack;
    TestTrue(TEXT("Applied parameters read from current document"), Subsystem->GetTowerDefenseParameters(ReadBack, &Error));
    TestEqual(TEXT("Two wave configuration is read"), ReadBack.Waves.Num(), 2);
    TestEqual(TEXT("Base life is applied"), ReadBack.BaseMaxHealth, Edited.BaseMaxHealth);
    TestEqual(TEXT("Initial resources are applied"), ReadBack.InitialResources, Edited.InitialResources);
    TestEqual(TEXT("Wave interval is applied"), ReadBack.WaveIntervalSeconds, Edited.WaveIntervalSeconds);
    TestTrue(TEXT("Defeat rule is preserved"), ReadBack.DefeatCondition == Edited.DefeatCondition);
    TestTrue(TEXT("Victory rule is preserved"), ReadBack.VictoryCondition == Edited.VictoryCondition);
    for (int32 Index = 0; Index < ReadBack.Waves.Num() && Edited.Waves.IsValidIndex(Index); ++Index)
    {
        const FAkUGCTowerDefenseWaveParameters& Expected = Edited.Waves[Index];
        const FAkUGCTowerDefenseWaveParameters& Actual = ReadBack.Waves[Index];
        TestEqual(TEXT("Enemy whitelist value is preserved"), Actual.EnemyPrefabId, Expected.EnemyPrefabId);
        TestEqual(TEXT("Enemy count is applied"), Actual.EnemyCount, Expected.EnemyCount);
        TestEqual(TEXT("Spawn interval is applied"), Actual.SpawnIntervalSeconds, Expected.SpawnIntervalSeconds);
        TestEqual(TEXT("Start delay is applied"), Actual.StartDelaySeconds, Expected.StartDelaySeconds);
        TestEqual(TEXT("Enemy health is applied"), Actual.EnemyMaxHealth, Expected.EnemyMaxHealth);
        TestEqual(TEXT("Enemy speed is applied"), Actual.EnemyMoveSpeed, Expected.EnemyMoveSpeed);
        TestEqual(TEXT("Goal damage is applied"), Actual.EnemyGoalDamage, Expected.EnemyGoalDamage);
    }
    const FString AppliedDocument = Snapshot();
    const uint64 RevisionBeforeRejected = Subsystem->GetDocumentRevision();
    FAkUGCTowerDefenseParameters Invalid = Edited;
    Invalid.Waves[0].EnemyPrefabId = TEXT("untrusted.enemy");
    TestFalse(TEXT("Unsupported enemy is rejected"), Subsystem->ApplyTowerDefenseParameters(Invalid, &Error));
    TestFalse(TEXT("Rejected apply returns readable error"), Error.IsEmpty());
    TestEqual(TEXT("Rejected enemy leaves document unchanged"), Snapshot(), AppliedDocument);
    Invalid = Edited;
    Invalid.Waves[0].EnemyCount = 0;
    TestFalse(TEXT("Invalid enemy count is rejected"), Subsystem->ApplyTowerDefenseParameters(Invalid, &Error));
    TestEqual(TEXT("Rejected count leaves document unchanged"), Snapshot(), AppliedDocument);
    Invalid = Edited;
    Invalid.Waves.SetNum(4);
    TestFalse(TEXT("More than three waves are rejected"), Subsystem->ApplyTowerDefenseParameters(Invalid, &Error));
    TestEqual(TEXT("Rejected wave count leaves revision unchanged"), Subsystem->GetDocumentRevision(), RevisionBeforeRejected);
    TestTrue(TEXT("One Undo restores all parameter changes"), Subsystem->Undo().bSucceeded);
    Form->Tick(FGeometry(), 1.0, 0.0f);
    TestEqual(TEXT("Undo refreshes simple form to original waves"), Form->GetDisplayedParameters().Waves.Num(), 3);
    TestEqual(TEXT("Undo refreshes simple form to original base life"), Form->GetDisplayedParameters().BaseMaxHealth, DefaultParameters.BaseMaxHealth);
    TestEqual(TEXT("Undo restores original full document"), Snapshot(), OriginalDocument);
    TestFalse(TEXT("Apply consumed exactly one history entry"), Subsystem->CanUndo());
    TestTrue(TEXT("Undo refreshes parameter readback"), Subsystem->GetTowerDefenseParameters(ReadBack, &Error));
    TestEqual(TEXT("Undo reads original three waves"), ReadBack.Waves.Num(), 3);
    TestEqual(TEXT("Undo reads original base life"), ReadBack.BaseMaxHealth, DefaultParameters.BaseMaxHealth);
    TestTrue(TEXT("Redo restores all parameter changes"), Subsystem->Redo().bSucceeded);
    Form->Tick(FGeometry(), 2.0, 0.0f);
    TestEqual(TEXT("Redo refreshes simple form wave count"), Form->GetDisplayedParameters().Waves.Num(), 2);
    TestEqual(TEXT("Redo refreshes simple form base life"), Form->GetDisplayedParameters().BaseMaxHealth, Edited.BaseMaxHealth);
    TestEqual(TEXT("Redo restores applied full document"), Snapshot(), AppliedDocument);
    const FString TestPath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation/CreatorStudioTowerDefenseParameters.json"));
    TestTrue(TEXT("Parameter project saves"), Subsystem->SaveProject(TestPath, &Error));
    Subsystem->CloseProject();
    if (Subsystem->LoadProject(TestPath, &Error))
    {
        TestEqual(TEXT("Save and reload preserve applied document"), Snapshot(), AppliedDocument);
        TestTrue(TEXT("Reload reads current parameters"), Subsystem->GetTowerDefenseParameters(ReadBack, &Error));
        TestEqual(TEXT("Reload preserves base life"), ReadBack.BaseMaxHealth, Edited.BaseMaxHealth);
        TestEqual(TEXT("Reload preserves resources"), ReadBack.InitialResources, Edited.InitialResources);
        FAkUGCTowerDefenseParameters OneWave = ReadBack;
        OneWave.Waves.SetNum(1);
        TestTrue(TEXT("One-wave project applies"), Subsystem->ApplyTowerDefenseParameters(OneWave, &Error));
        TestTrue(TEXT("One-wave project reads"), Subsystem->GetTowerDefenseParameters(ReadBack, &Error));
        TestEqual(TEXT("One-wave lower bound is supported"), ReadBack.Waves.Num(), 1);
    }
    else
    {
        AddError(FString::Printf(TEXT("Parameter reload failed: %s"), *Error));
    }
    Subsystem->CloseProject();
    IFileManager::Get().Delete(*TestPath, false, true);
    TestFalse(TEXT("Closed project cannot read parameters"), Subsystem->GetTowerDefenseParameters(ReadBack, &Error));
    TestFalse(TEXT("Closed project cannot apply parameters"), Subsystem->ApplyTowerDefenseParameters(Edited, &Error));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAkUGCCreatorPublishTest, "AkUGC.Editor.CreatorStudio.PublishSnapshot",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAkUGCCreatorPublishTest::RunTest(const FString& Parameters)
{
    auto* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UAkUGCEditorSubsystem>() : nullptr;
    if (!Subsystem) { AddError(TEXT("Missing Creator subsystem.")); return false; }
    FString Error;
    if (!Subsystem->NewTowerDefenseProject(&Error)) { AddError(Error); return false; }
    const uint64 Revision = Subsystem->GetDocumentRevision();
    const FString Hash = FAkUGCLogicPackHasher::HashDocument(Subsystem->GetDocument());
    const TSharedRef<SAkUGCWorkLibraryPanel> Panel = SNew(SAkUGCWorkLibraryPanel, Subsystem);
    const FString Root = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation/CreatorPublish"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
#if !PLATFORM_WINDOWS
    AddInfo(TEXT("Protected publisher identity is Win64-only; Creator publish snapshot is skipped on this platform."));
    Subsystem->CloseProject();
    IFileManager::Get().DeleteDirectory(*Root, false, true);
    return true;
#endif
    const FString PublisherDirectory = FPaths::Combine(Root, TEXT("Publisher"));
    FString PublicKey, Share;
    FAkUGCWorkEntry Entry;
    TestTrue(TEXT("Protected publisher identity creates"), FAkUGCPublisherKeyStore(PublisherDirectory).GetOrCreatePublicKey(PublicKey, &Error));
    TestTrue(TEXT("Exact Creator snapshot publishes"), FAkUGCWorkLibrary(FPaths::Combine(Root, TEXT("Library"))).PublishWithPublisherIdentity(
        Subsystem->GetDocument(), PublisherDirectory, FPaths::Combine(Root, TEXT("Share")), Entry, Share, &Error));
    TestEqual(TEXT("Published snapshot uses protected publisher identity"), Entry.TrustedPublicKey, PublicKey);
    const auto Loaded = FAkUGCWorkLibrary(FPaths::Combine(Root, TEXT("Library"))).Load(Entry.ReleaseId);
    TestTrue(TEXT("Published Creator work re-verifies"), Loaded.bSucceeded);
    TestEqual(TEXT("Published snapshot matches Creator"), FAkUGCLogicPackHasher::HashDocument(Loaded.Pack.Document), Hash);
    TestEqual(TEXT("Publishing preserves revision"), Subsystem->GetDocumentRevision(), Revision);
    TestFalse(TEXT("Publishing does not create undo history"), Subsystem->CanUndo());
    Subsystem->CloseProject();
    IFileManager::Get().DeleteDirectory(*Root, false, true);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAkUGCCopilotApplyTest, "AkUGC.Editor.Copilot.PreviewAtomicUndo",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAkUGCCopilotApplyTest::RunTest(const FString& Parameters)
{
    auto* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UAkUGCEditorSubsystem>() : nullptr;
    if (!Subsystem) { AddError(TEXT("Missing Creator subsystem.")); return false; }
    FString Error;
    if (!Subsystem->NewTowerDefenseProject(&Error)) { AddError(Error); return false; }
    const TSharedRef<SAkUGCCopilotPanel> Panel = SNew(SAkUGCCopilotPanel, Subsystem);
    const FString Before = FAkUGCLogicPackHasher::HashDocument(Subsystem->GetDocument());
    FAkUGCTowerDefenseParameters Proposed;
    Proposed.Waves.SetNum(2); Proposed.InitialResources = 300; Proposed.BaseMaxHealth = 1500;
    FString Json;
    FAkUGCTowerDefenseTemplate::SerializeParameters(Proposed, Json);
    FAkUGCCopilotPreview Preview;
    TestTrue(TEXT("Proposal precompiles on copy"), FAkUGCCopilotValidation::Preview(Subsystem->GetDocument(), Json, Preview, &Error));
    TestEqual(TEXT("Preview preserves authored document"), FAkUGCLogicPackHasher::HashDocument(Subsystem->GetDocument()), Before);
    TestFalse(TEXT("Preview creates no undo entry"), Subsystem->CanUndo());
    TestTrue(TEXT("Explicit proposal applies one transaction"), Subsystem->ApplyCopilotProposal(Preview, Subsystem->GetDocumentRevision(), &Error));
    const FString After = FAkUGCLogicPackHasher::HashDocument(Subsystem->GetDocument());
    TestTrue(TEXT("Preview becomes stale after apply"), !FAkUGCCopilotValidation::IsCurrent(Subsystem->GetDocument(), Preview));
    TestTrue(TEXT("Single Undo restores whole proposal"), Subsystem->Undo().bSucceeded);
    TestEqual(TEXT("Undo restores exact source document"), FAkUGCLogicPackHasher::HashDocument(Subsystem->GetDocument()), Before);
    TestFalse(TEXT("Only one proposal history entry"), Subsystem->CanUndo());
    TestTrue(TEXT("Redo restores all proposal changes"), Subsystem->Redo().bSucceeded);
    TestEqual(TEXT("Redo restores exact result"), FAkUGCLogicPackHasher::HashDocument(Subsystem->GetDocument()), After);
    Subsystem->CloseProject();
    return true;
}
#endif

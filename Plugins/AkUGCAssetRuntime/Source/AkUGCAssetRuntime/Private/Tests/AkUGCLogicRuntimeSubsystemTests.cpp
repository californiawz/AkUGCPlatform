#include "Misc/AutomationTest.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Prefab/AkUGCPrefabRegistry.h"
#include "Scene/AkUGCSceneRuntime.h"
#include "Session/AkUGCDocumentRuntimeSession.h"
#include "Subsystem/AkUGCLogicRuntimeSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCLogicRuntimeSubsystemTest,
    "AkUGC.Runtime.Logic.GameStartMessage",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCLogicRuntimeSubsystemTest::RunTest(const FString& Parameters)
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

    UAkUGCLogicRuntimeSubsystem* Subsystem = World->GetSubsystem<UAkUGCLogicRuntimeSubsystem>();
    TestNotNull(TEXT("Logic Runtime world subsystem exists"), Subsystem);
    if (!Subsystem)
    {
        GEngine->DestroyWorldContext(World);
        World->DestroyWorld(false);
        return false;
    }

    FAkUGCLogicNode Start;
    Start.NodeId = FGuid::NewGuid();
    Start.Type = EAkUGCLogicNodeType::GameStart;
    FAkUGCLogicNode Message;
    Message.NodeId = FGuid::NewGuid();
    Message.Type = EAkUGCLogicNodeType::Message;
    Message.Message = TEXT("Runtime ready");
    FAkUGCLogicConnection Connection;
    Connection.SourceNodeId = Start.NodeId;
    Connection.TargetNodeId = Message.NodeId;
    FAkUGCLogicGraph Graph;
    Graph.Nodes = {Message, Start};
    Graph.Connections.Add(Connection);

    const FAkUGCLogicRuntimeResult RunResult = Subsystem->RunGameStart(Graph);
    TestTrue(TEXT("World subsystem runs Game Start graph"), RunResult.bSucceeded);
    TestEqual(TEXT("World subsystem executes two instructions"), RunResult.ExecutedInstructionCount, 2);
    TestEqual(TEXT("World subsystem records one message"), Subsystem->GetEmittedMessages().Num(), 1);
    if (Subsystem->GetEmittedMessages().Num() == 1)
    {
        TestEqual(TEXT("Runtime message text is observable"),
            Subsystem->GetEmittedMessages()[0].Message,
            FString(TEXT("Runtime ready")));
        TestEqual(TEXT("Runtime message source is observable"),
            Subsystem->GetEmittedMessages()[0].SourceNodeId,
            Message.NodeId);
    }

    Subsystem->ResetLogicRuntime();
    TestTrue(TEXT("Reset clears emitted messages"), Subsystem->GetEmittedMessages().IsEmpty());

    FAkUGCProjectDocument Document;
    Document.Manifest.ProjectId = FGuid::NewGuid();
    Document.Manifest.DisplayName = TEXT("Logic Runtime Integration");
    Document.Manifest.TemplateId = TEXT("official.tower_defense");
    FAkUGCSceneDocument& Scene = Document.Scenes.AddDefaulted_GetRef();
    Scene.SceneId = FGuid::NewGuid();
    Scene.DisplayName = TEXT("Main");
    Scene.LogicGraph = Graph;
    FAkUGCPrefabRegistry Registry;
    {
        FAkUGCSceneRuntime SceneRuntime(World);
        FAkUGCDocumentRuntimeSession Session(SceneRuntime, Registry, Scene.SceneId);
        TestTrue(TEXT("Scene session initialization runs Game Start"), Session.Initialize(Document).bSucceeded);
        TestEqual(TEXT("Automatic Game Start emits the scene message"), Subsystem->GetEmittedMessages().Num(), 1);
    }
    TestTrue(TEXT("Scene unload clears Logic Runtime messages"), Subsystem->GetEmittedMessages().IsEmpty());

    Graph.Nodes[0].Message.Reset();
    TestFalse(TEXT("World subsystem rejects invalid graph"), Subsystem->RunGameStart(Graph).bSucceeded);
    TestTrue(TEXT("Failed run does not retain previous messages"), Subsystem->GetEmittedMessages().IsEmpty());

    GEngine->DestroyWorldContext(World);
    World->DestroyWorld(false);
    return true;
}

#endif

#include "Misc/AutomationTest.h"
#include "Game/AkUGCRoom.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Game/AkUGCGameMode.h"
#include "Game/AkUGCGameState.h"
#include "Prefab/AkUGCOfficialPrefabCatalog.h"
#include "Template/AkUGCTowerDefenseTemplate.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAkUGCRoomGateTest, "AkUGC.Runtime.Room.CapacityReadyAndHost",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ServerContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCRoomGateTest::RunTest(const FString& Parameters)
{
    FAkUGCRoomRoster Room;
    const int32 Host = Room.Join();
    TestFalse(TEXT("Single member cannot start"), Room.CanStart(Host));
    const int32 Other = Room.Join();
    TestFalse(TEXT("Not ready room cannot start"), Room.CanStart(Host));
    Room.SetReady(Host, true); Room.SetReady(Other, true);
    TestTrue(TEXT("Ready host can start"), Room.CanStart(Host));
    TestFalse(TEXT("Ready non-host cannot start"), Room.CanStart(Other));
    Room.Join(); Room.Join();
    TestEqual(TEXT("Fifth join rejected"), Room.Join(), 0);
    TestFalse(TEXT("Unknown member cannot modify ready"), Room.SetReady(999, true));
    Room.Leave(Host);
    TestTrue(TEXT("Host transfers to next member"), Room.IsHost(Other));
    const int32 Rejoined = Room.Join();
    TestTrue(TEXT("Rejoin allocates fresh identity"), Rejoined != Host);
    TestFalse(TEXT("Rejoin does not reclaim host"), Room.IsHost(Rejoined));
    Room.ResetReady();
    TestFalse(TEXT("New round requires fresh readiness"), Room.CanStart(Other));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAkUGCRoomEndpointTest, "AkUGC.Runtime.Room.StrictEndpoint",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ServerContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCRoomEndpointTest::RunTest(const FString& Parameters)
{
    FString Out;
    TestTrue(TEXT("Loopback accepted for local testing"), FAkUGCRoomRoster::ValidateEndpoint(TEXT("127.0.0.1:7777"), Out));
    TestEqual(TEXT("Endpoint canonicalized"), Out, FString(TEXT("127.0.0.1:7777")));
    for (const FString& Input : {TEXT("127.0.0.1:0"), TEXT("127.0.0.1:65536"), TEXT("0.0.0.0:7777"), TEXT("224.0.0.1:7777"),
        TEXT("http://127.0.0.1:7777"), TEXT("127.0.0.1:7777?listen"), TEXT("127.0.0.1:7777;quit"), TEXT("127..1:7777"), TEXT("256.0.0.1:7777")})
    {
        TestFalse(TEXT("Invalid or injected travel options rejected"), FAkUGCRoomRoster::ValidateEndpoint(Input, Out));
        TestTrue(TEXT("Invalid endpoint clears result"), Out.IsEmpty());
    }
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAkUGCRoomAuthorityGateTest, "AkUGC.Runtime.Room.AuthorityEntryGates",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCRoomAuthorityGateTest::RunTest(const FString& Parameters)
{
    UWorld* World = NewObject<UWorld>(GetTransientPackage(), NAME_None, RF_Transient);
    World->WorldType = EWorldType::Game;
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    World->InitializeNewWorld(UWorld::InitializationValues().InitializeScenes(false).AllowAudioPlayback(false)
        .CreatePhysicsScene(false).CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false));
    AAkUGCGameMode* Mode = World->SpawnActor<AAkUGCGameMode>();
    World->SetGameState(World->SpawnActor<AAkUGCGameState>());
    FString Error;
    Mode->InitGame(TEXT("Phase0"), TEXT("?room=1"), Error);
    FAkUGCPrefabRegistry Registry;
    FAkUGCProjectDocument Document;
    FAkUGCOfficialPrefabCatalog::RegisterTowerDefense(Registry);
    FAkUGCTowerDefenseTemplate::CreateDefaultDocument(Registry, Document);
    TestTrue(TEXT("Room mode selected"), Mode->IsRoomMode());
    TestFalse(TEXT("Public authority initializer cannot bypass lobby"), Mode->InitializeAuthoritySession(Document, &Error));
    TestFalse(TEXT("Solo preparation cannot bypass room"), Mode->PrepareSoloProject(Document, &Error));
    TestFalse(TEXT("Solo start cannot bypass room"), Mode->StartSoloMatch(&Error));
    TestFalse(TEXT("Unknown player cannot ready"), Mode->SetMemberReady(nullptr, true));
    TestFalse(TEXT("Unknown player cannot start"), Mode->StartRoomMatch(nullptr));
    TestFalse(TEXT("Unknown player cannot return"), Mode->ReturnRoomToPreparation(nullptr));
    TestFalse(TEXT("No rejected call created session"), Mode->HasAuthoritySession());
    GEngine->DestroyWorldContext(World); World->DestroyWorld(false);
    return true;
}
#endif

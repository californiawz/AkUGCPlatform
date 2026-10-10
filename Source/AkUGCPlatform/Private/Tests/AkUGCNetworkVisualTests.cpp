#include "Misc/AutomationTest.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/NetDriver.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Entity/AkUGCRuntimeEntityActor.h"
#include "Game/AkUGCGameMode.h"
#include "Game/AkUGCGameState.h"
#include "GameFramework/PlayerController.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Pack/AkUGCLogicPackCodec.h"
#include "Pack/AkUGCLogicPackHasher.h"
#include "Prefab/AkUGCOfficialPrefabCatalog.h"
#include "Prefab/AkUGCPrefabRegistry.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AkUGCNetworkVisualTest
{
    UWorld* FindNetworkWorld(ENetMode Mode)
    {
        for (const FWorldContext& Context : GEngine->GetWorldContexts())
        {
            UWorld* World = Context.World();
            if (World && World->IsGameWorld() && World->GetNetMode() == Mode && World->GetNetDriver()) { return World; }
        }
        return nullptr;
    }

    class FObserveNetwork final : public IAutomationLatentCommand
    {
    public:
        FObserveNetwork(FAutomationTestBase* InTest, bool bInServer)
            : Test(InTest), bServer(bInServer), Started(FPlatformTime::Seconds())
        {
            FAkUGCOfficialPrefabCatalog::RegisterTowerDefense(Registry);
        }

        virtual bool Update() override
        {
            if (FPlatformTime::Seconds() - Started > 100.0)
            {
                Test->AddError(TEXT("Timed out waiting for real network replication, movement and terminal state."));
                return true;
            }
            UWorld* World = FindNetworkWorld(bServer ? NM_DedicatedServer : NM_Client);
            if (!World) { return false; }
            AAkUGCGameState* State = World->GetGameState<AAkUGCGameState>();
            if (!State || State->GetResources() != 321 || State->GetBaseHealthMaximum() != 1000.0) { return false; }
            if (bServer)
            {
                AAkUGCGameMode* Mode = World->GetAuthGameMode<AAkUGCGameMode>();
                if (!Mode || !Mode->HasAuthoritySession() || World->GetNetDriver()->ClientConnections.Num() < 2) { return false; }
            }
            else if (!World->GetNetDriver()->ServerConnection || !World->GetFirstPlayerController()
                || !World->GetFirstPlayerController()->IsLocalController()) { return false; }

            TSet<FGuid> Ids;
            TSet<FName> Types;
            AAkUGCRuntimeEntityActor* Enemy = nullptr;
            const FGuid ExpectedEnemy = FGuid::NewDeterministicGuid(TEXT("ugc-runtime-spawn-0"));
            for (TActorIterator<AAkUGCRuntimeEntityActor> It(World); It; ++It)
            {
                AAkUGCRuntimeEntityActor* Actor = *It;
                if (!Actor->GetEntityId().IsValid() || Actor->GetPrefabId().IsNone()) { return false; }
                if (Ids.Contains(Actor->GetEntityId()))
                {
                    Test->AddError(TEXT("Replicated entity GUID is duplicated."));
                    return true;
                }
                Ids.Add(Actor->GetEntityId());
                Types.Add(Actor->GetPrefabId());
                if (Actor->GetEntityId() == ExpectedEnemy) { Enemy = Actor; }
                if (bServer)
                {
                    if (Actor->VisualMesh->GetStaticMesh())
                    {
                        Test->AddError(TEXT("Dedicated Server loaded a visual Mesh."));
                        return true;
                    }
                }
                else
                {
                    const FSoftObjectPath* Path = Registry.ResolveAsset(Actor->GetPrefabId(), TEXT("Win64"));
                    const UStaticMesh* Mesh = Actor->VisualMesh->GetStaticMesh().Get();
                    if (!Path || !Path->IsValid() || !Mesh || Mesh->GetPathName() != Path->ToString()
                        || !Actor->VisualMesh->IsRegistered() || !Actor->VisualMesh->IsVisible() || Actor->IsHidden()) { return false; }
                }
            }
            const TArray<FName> Required = {TEXT("official.gameplay.base"), TEXT("official.gameplay.goal"),
                TEXT("official.tower.basic"), TEXT("official.gameplay.enemy_spawn"), TEXT("official.gameplay.path_node")};
            for (FName Type : Required) { if (!Types.Contains(Type)) { return false; } }
            if (bServer)
            {
                Test->TestTrue(TEXT("Both real client connections reached the Dedicated Server"), World->GetNetDriver()->ClientConnections.Num() >= 2);
                UE_LOG(LogTemp, Display, TEXT("[AkUGCNetworkVisual] ServerPassed Connections=%d Entities=%d Resources=%d"),
                    World->GetNetDriver()->ClientConnections.Num(), Ids.Num(), State->GetResources());
                return true;
            }
            if (Enemy)
            {
                if (Enemy->HasAuthority())
                {
                    Test->AddError(TEXT("Client entity has authority; this is not a replication test."));
                    return true;
                }
                if (!bObservedEnemy)
                {
                    bObservedEnemy = true;
                    InitialLocation = Enemy->GetActorLocation();
                    UE_LOG(LogTemp, Display, TEXT("[AkUGCNetworkVisual] ObservedEnemy Id=%s Entities=%d Resources=%d"),
                        *ExpectedEnemy.ToString(EGuidFormats::Digits), Ids.Num(), State->GetResources());
                }
                bObservedMovement |= FVector::Distance(InitialLocation, Enemy->GetActorLocation()) > 10.0;
                return false;
            }
            if (!bObservedEnemy || !bObservedMovement || State->GetMatchResult() != EAkUGCTowerDefenseMatchResult::Victory
                || State->GetActiveEnemyCount() != 0 || State->GetBaseHealthCurrent() != 993.0) { return false; }
            Test->TestTrue(TEXT("Replicated enemy moved before server destruction"), bObservedMovement);
            UE_LOG(LogTemp, Display, TEXT("[AkUGCNetworkVisual] ClientPassed Id=%s Base=993 Resources=321 Destroyed=1 Moved=1"),
                *ExpectedEnemy.ToString(EGuidFormats::Digits));
            return true;
        }

    private:
        FAutomationTestBase* Test;
        bool bServer;
        double Started;
        FAkUGCPrefabRegistry Registry;
        bool bObservedEnemy = false;
        bool bObservedMovement = false;
        FVector InitialLocation = FVector::ZeroVector;
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAkUGCNetworkClientVisualTest,
    "AkUGC.Runtime.Network.ClientVisualReplication", EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCNetworkClientVisualTest::RunTest(const FString& Parameters)
{
    if (!FParse::Param(FCommandLine::Get(), TEXT("AkUGCNetworkVisual")))
    {
        AddWarning(TEXT("Real network visual test requires -AkUGCNetworkVisual and the dedicated fixture server."));
        return true;
    }
    ADD_LATENT_AUTOMATION_COMMAND(AkUGCNetworkVisualTest::FObserveNetwork(this, false));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAkUGCNetworkServerVisualTest,
    "AkUGC.Runtime.Network.ServerVisualBaseline", EAutomationTestFlags::ServerContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCNetworkServerVisualTest::RunTest(const FString& Parameters)
{
    if (!FParse::Param(FCommandLine::Get(), TEXT("AkUGCNetworkVisual")))
    {
        AddWarning(TEXT("Real network visual test requires -AkUGCNetworkVisual and two remote clients."));
        return true;
    }
    FString PackPath, Json, Error, Canonical;
    FAkUGCLogicPack Pack;
    if (FParse::Value(FCommandLine::Get(), TEXT("LogicPack="), PackPath)
        && FFileHelper::LoadFileToString(Json, *PackPath)
        && FAkUGCLogicPackCodec::Deserialize(Json, Pack, &Error)
        && FAkUGCLogicPackHasher::DeterministicSerialize(Pack.Document, Canonical, &Error))
    {
        FFileHelper::SaveStringToFile(Canonical, *(PackPath + TEXT(".server.canonical.json")));
    }
    ADD_LATENT_AUTOMATION_COMMAND(AkUGCNetworkVisualTest::FObserveNetwork(this, true));
    return true;
}

#endif

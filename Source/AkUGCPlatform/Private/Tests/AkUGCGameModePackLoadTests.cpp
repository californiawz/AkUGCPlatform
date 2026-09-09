#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"

#include "AkUGCPlayableSceneFactory.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Game/AkUGCGameMode.h"
#include "Game/AkUGCGameState.h"
#include "Gameplay/AkUGCTowerDefenseStateHasher.h"
#include "Subsystem/AkUGCLogicRuntimeSubsystem.h"
#include "Document/AkUGCDocument.h"
#include "Pack/AkUGCLogicPack.h"
#include "Pack/AkUGCLogicPackBuilder.h"
#include "Pack/AkUGCLogicPackCodec.h"
#include "Pack/AkUGCLogicPackLoader.h"
#include "Pack/AkUGCLogicPackSignature.h"
#include "Prefab/AkUGCOfficialPrefabCatalog.h"
#include "Prefab/AkUGCPrefabRegistry.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AkUGCGameModePackLoadTest
{
    struct FScopedTempFile
    {
        FString Path;

        explicit FScopedTempFile(const TCHAR* Extension)
        {
            Path = FPaths::CreateTempFilename(
                *FPaths::ProjectSavedDir(),
                TEXT("AkUGCGameModePack"),
                Extension);
        }

        ~FScopedTempFile()
        {
            if (!Path.IsEmpty())
            {
                IFileManager::Get().Delete(*Path, /*bRequireExists=*/false, /*bEvenReadOnly=*/true);
            }
        }
    };

    UWorld* CreateTestWorld()
    {
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
        return World;
    }

    void DestroyTestWorld(UWorld* World)
    {
        GEngine->DestroyWorldContext(World);
        World->DestroyWorld(false);
    }

    bool WriteSignedPackFile(
        const FAkUGCProjectDocument& Document,
        const FAkUGCLogicPackKeyPair& KeyPair,
        const FString& FilePath,
        FString* OutError)
    {
        const FAkUGCLogicPackBuildResult Build = FAkUGCLogicPackBuilder::Build(Document);
        if (!Build.bSucceeded)
        {
            if (OutError)
            {
                *OutError = Build.ErrorMessage;
            }
            return false;
        }

        FAkUGCLogicPack Pack = Build.Pack;
        if (!FAkUGCLogicPackSigner::Sign(Pack.Manifest, KeyPair.PrivateKey, Pack.Signature, OutError))
        {
            return false;
        }

        FString Json;
        if (!FAkUGCLogicPackCodec::Serialize(Pack, Json, OutError))
        {
            return false;
        }

        return FFileHelper::SaveStringToFile(Json, *FilePath);
    }

    FAkUGCTowerDefenseFinalState CollectFinalState(
        UAkUGCLogicRuntimeSubsystem* Logic,
        AAkUGCGameState* GameState)
    {
        FAkUGCTowerDefenseFinalState State;
        const FAkUGCWaveRuntimeSnapshot Wave = Logic->GetWaveRuntimeState();
        State.WaveState = Wave.State;
        State.MatchResult = Wave.Result;
        State.CurrentWaveIndex = Wave.CurrentWaveIndex;
        State.CurrentWaveId = Wave.CurrentWaveId;
        State.TotalWaveCount = Wave.TotalWaveCount;
        State.BaseCurrentHealth = GameState->GetBaseHealthCurrent();
        State.BaseMaximumHealth = GameState->GetBaseHealthMaximum();
        State.ActiveEnemyCount = GameState->GetActiveEnemyCount();
        State.Messages = Logic->GetEmittedMessages();
        State.Spawns = Logic->GetSpawnedEntities();
        State.Goals = Logic->GetGoalReachedEntities();
        State.Damages = Logic->GetDamageEvents();
        State.Deaths = Logic->GetDeathEvents();
        return State;
    }

    /** 基于标准场景加入强塔并压缩波次节奏，使其在数秒内收敛到 Victory 终局。 */
    bool MakeVictoryDocument(
        const FAkUGCProjectDocument& Standard,
        FAkUGCProjectDocument& OutVictory,
        FString* OutError)
    {
        FAkUGCPrefabRegistry Registry;
        if (!FAkUGCOfficialPrefabCatalog::RegisterTowerDefense(Registry, OutError))
        {
            return false;
        }

        OutVictory = Standard;
        FAkUGCSceneDocument& Scene = OutVictory.Scenes[0];

        // 每波 1 敌，短延迟与短间隔，让三波在数秒内跑完到 Victory。
        FAkUGCComponentRecord* SpawnConfig = nullptr;
        for (FAkUGCEntityRecord& Entity : Scene.Entities)
        {
            SpawnConfig = Entity.Components.FindByPredicate([](const FAkUGCComponentRecord& Component)
            {
                return Component.TypeId == TEXT("tower_defense.spawn");
            });
            if (SpawnConfig)
            {
                SpawnConfig->Properties.FindChecked(TEXT("enemyCount")).IntegerValue = 1;
                break;
            }
        }
        if (!SpawnConfig)
        {
            if (OutError) { *OutError = TEXT("Missing enemy_spawn component in standard scene"); }
            return false;
        }

        Scene.Ruleset.WaveIntervalSeconds = 0.2;
        for (FAkUGCTowerDefenseWave& Wave : Scene.Ruleset.Waves)
        {
            Wave.StartDelaySeconds = 0.1;
        }

        // 强塔：1 击必杀，射程覆盖整条路径，确保全部波次清空后 Victory。
        FAkUGCEntityRecord Tower;
        if (!Registry.CreateEntityRecord(
            TEXT("official.tower.basic"),
            FGuid(0xE0000010, 0, 0, 0),
            FTransform(FVector(600.0, 50.0, 0.0)),
            Tower,
            OutError))
        {
            return false;
        }
        if (FAkUGCComponentRecord* TowerConfig = Tower.Components.FindByPredicate(
            [](const FAkUGCComponentRecord& Component)
            { return Component.TypeId == TEXT("tower_defense.tower"); }))
        {
            TowerConfig->Properties.FindChecked(TEXT("attackRange")).NumberValue = 1000.0;
            TowerConfig->Properties.FindChecked(TEXT("attackInterval")).NumberValue = 0.1;
            TowerConfig->Properties.FindChecked(TEXT("attackDamage")).NumberValue = 1000.0;
        }
        Scene.Entities.Add(Tower);

        return true;
    }

    /**
     * 在独立 World 中把签名 Pack 推进到终局并返回玩法层最终状态哈希。
     * bSmallDelta 为 true 时按 0.25s 步进推进，否则单次大步长推进。
     */
    bool RunTerminalHash(
        const FAkUGCProjectDocument& Document,
        const FAkUGCLogicPackKeyPair& KeyPair,
        double AdvanceSeconds,
        bool bSmallDelta,
        FString& OutHash,
        FString* OutError)
    {
        FScopedTempFile TempFile(TEXT(".json"));
        if (!WriteSignedPackFile(Document, KeyPair, TempFile.Path, OutError))
        {
            return false;
        }

        UWorld* World = CreateTestWorld();
        AAkUGCGameMode* GameMode = World->SpawnActor<AAkUGCGameMode>();
        AAkUGCGameState* GameState = World->SpawnActor<AAkUGCGameState>();
        if (!GameMode || !GameState)
        {
            if (OutError) { *OutError = TEXT("Failed to spawn GameMode or GameState"); }
            DestroyTestWorld(World);
            return false;
        }

        if (!GameMode->LoadAndInitializeAuthoritySession(TempFile.Path, KeyPair.PublicKey, OutError))
        {
            DestroyTestWorld(World);
            return false;
        }

        UAkUGCLogicRuntimeSubsystem* Logic = World->GetSubsystem<UAkUGCLogicRuntimeSubsystem>();
        if (!Logic)
        {
            if (OutError) { *OutError = TEXT("Logic runtime subsystem unavailable"); }
            DestroyTestWorld(World);
            return false;
        }

        if (bSmallDelta)
        {
            const double Step = 0.25;
            double Remaining = AdvanceSeconds;
            while (Remaining > 0.0)
            {
                const double Slice = FMath::Min(Step, Remaining);
                if (!Logic->AdvanceLogicTime(Slice).bSucceeded)
                {
                    if (OutError) { *OutError = TEXT("Small-delta advance failed"); }
                    DestroyTestWorld(World);
                    return false;
                }
                Remaining -= Slice;
            }
        }
        else
        {
            if (!Logic->AdvanceLogicTime(AdvanceSeconds).bSucceeded)
            {
                if (OutError) { *OutError = TEXT("Large-delta advance failed"); }
                DestroyTestWorld(World);
                return false;
            }
        }

        GameMode->ProjectStateToGameState(GameState);
        const FAkUGCTowerDefenseFinalState State = CollectFinalState(Logic, GameState);
        OutHash = FAkUGCTowerDefenseStateHasher::HashFinalState(State);

        DestroyTestWorld(World);
        return true;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCGameModePackLoadTest,
    "AkUGC.Runtime.GameMode.PackLoad",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCGameModePackLoadTest::RunTest(const FString& Parameters)
{
    if (!GEngine)
    {
        AddError(TEXT("Engine is not available."));
        return false;
    }

    FAkUGCProjectDocument Document;
    FString Error;
    TestTrue(*Error, AkUGCPlayableSceneFactory::MakePlayableTowerDefenseDocument(Document, &Error));

    FAkUGCLogicPackKeyPair KeyPair;
    TestTrue(*Error, FAkUGCLogicPackSigner::GenerateKeyPair(KeyPair, &Error));

    AkUGCGameModePackLoadTest::FScopedTempFile TempFile(TEXT(".json"));
    TestTrue(*Error, AkUGCGameModePackLoadTest::WriteSignedPackFile(Document, KeyPair, TempFile.Path, &Error));

    UWorld* World = AkUGCGameModePackLoadTest::CreateTestWorld();
    AAkUGCGameMode* GameMode = World->SpawnActor<AAkUGCGameMode>();
    TestNotNull(TEXT("GameMode spawns in test world"), GameMode);
    if (!GameMode)
    {
        AkUGCGameModePackLoadTest::DestroyTestWorld(World);
        return false;
    }

    TestFalse(TEXT("GameMode starts without authority session"), GameMode->HasAuthoritySession());

    // 签名 Pack 成功加载并初始化权威会话。
    TestTrue(*Error, GameMode->LoadAndInitializeAuthoritySession(TempFile.Path, KeyPair.PublicKey, &Error));
    TestTrue(TEXT("Authority session is initialized from signed pack"), GameMode->HasAuthoritySession());

    // 重复初始化被拒绝。
    const bool bSecond = GameMode->LoadAndInitializeAuthoritySession(TempFile.Path, KeyPair.PublicKey, &Error);
    TestFalse(TEXT("Duplicate initialization is rejected"), bSecond);
    TestTrue(TEXT("Duplicate rejection carries an error"), Error.Contains(TEXT("already initialized")));

    AkUGCGameModePackLoadTest::DestroyTestWorld(World);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCGameModePackRejectTest,
    "AkUGC.Runtime.GameMode.PackReject",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCGameModePackRejectTest::RunTest(const FString& Parameters)
{
    if (!GEngine)
    {
        AddError(TEXT("Engine is not available."));
        return false;
    }

    FAkUGCProjectDocument Document;
    FString Error;
    TestTrue(*Error, AkUGCPlayableSceneFactory::MakePlayableTowerDefenseDocument(Document, &Error));

    FAkUGCLogicPackKeyPair KeyPair;
    FAkUGCLogicPackKeyPair OtherKeyPair;
    TestTrue(*Error, FAkUGCLogicPackSigner::GenerateKeyPair(KeyPair, &Error));
    TestTrue(*Error, FAkUGCLogicPackSigner::GenerateKeyPair(OtherKeyPair, &Error));

    // 1. 未签名 Pack 文件被拒绝（验签加载要求签名）。
    const FAkUGCLogicPackBuildResult Build = FAkUGCLogicPackBuilder::Build(Document);
    TestTrue(*Build.ErrorMessage, Build.bSucceeded);
    FString UnsignedJson;
    TestTrue(*Error, FAkUGCLogicPackCodec::Serialize(Build.Pack, UnsignedJson, &Error));
    AkUGCGameModePackLoadTest::FScopedTempFile UnsignedFile(TEXT(".json"));
    TestTrue(TEXT("Unsigned pack is written"), FFileHelper::SaveStringToFile(UnsignedJson, *UnsignedFile.Path));

    UWorld* World = AkUGCGameModePackLoadTest::CreateTestWorld();
    AAkUGCGameMode* GameMode = World->SpawnActor<AAkUGCGameMode>();
    TestNotNull(TEXT("GameMode spawns in test world"), GameMode);
    if (!GameMode)
    {
        AkUGCGameModePackLoadTest::DestroyTestWorld(World);
        return false;
    }

    TestFalse(*Error, GameMode->LoadAndInitializeAuthoritySession(UnsignedFile.Path, KeyPair.PublicKey, &Error));
    TestFalse(TEXT("Unsigned pack does not initialize a session"), GameMode->HasAuthoritySession());

    // 2. 缺失文件被拒绝。
    TestFalse(*Error, GameMode->LoadAndInitializeAuthoritySession(
        TEXT("E:/__definitely_missing__.json"), KeyPair.PublicKey, &Error));
    TestFalse(TEXT("Missing file does not initialize a session"), GameMode->HasAuthoritySession());

    // 3. 错误公钥被拒绝。
    AkUGCGameModePackLoadTest::FScopedTempFile SignedFile(TEXT(".json"));
    TestTrue(*Error, AkUGCGameModePackLoadTest::WriteSignedPackFile(Document, KeyPair, SignedFile.Path, &Error));
    TestFalse(*Error, GameMode->LoadAndInitializeAuthoritySession(SignedFile.Path, OtherKeyPair.PublicKey, &Error));
    TestFalse(TEXT("Wrong trusted key does not initialize a session"), GameMode->HasAuthoritySession());

    // 4. 签名合法但内容非法（缺 base）的 Pack 在运行时被拒绝。
    FAkUGCProjectDocument InvalidDocument;
    TestTrue(*Error, AkUGCPlayableSceneFactory::MakePlayableTowerDefenseDocument(InvalidDocument, &Error));
    InvalidDocument.Scenes[0].Entities.RemoveAll([](const FAkUGCEntityRecord& Entity)
    {
        return Entity.PrefabId == TEXT("official.gameplay.base");
    });
    AkUGCGameModePackLoadTest::FScopedTempFile InvalidFile(TEXT(".json"));
    TestTrue(*Error, AkUGCGameModePackLoadTest::WriteSignedPackFile(InvalidDocument, KeyPair, InvalidFile.Path, &Error));
    TestFalse(*Error, GameMode->LoadAndInitializeAuthoritySession(InvalidFile.Path, KeyPair.PublicKey, &Error));
    TestFalse(TEXT("Content-invalid pack does not initialize a session"), GameMode->HasAuthoritySession());
    TestTrue(TEXT("Content-invalid rejection triggers tower defense validation"),
        Error.Contains(TEXT("Tower defense")) && Error.Contains(TEXT("base")));

    AkUGCGameModePackLoadTest::DestroyTestWorld(World);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCGameModeJoinInProgressMidWaveTest,
    "AkUGC.Runtime.GameMode.JoinInProgressMidWave",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCGameModeJoinInProgressMidWaveTest::RunTest(const FString& Parameters)
{
    if (!GEngine)
    {
        AddError(TEXT("Engine is not available."));
        return false;
    }

    FAkUGCProjectDocument Document;
    FString Error;
    TestTrue(*Error, AkUGCPlayableSceneFactory::MakePlayableTowerDefenseDocument(Document, &Error));

    FAkUGCLogicPackKeyPair KeyPair;
    TestTrue(*Error, FAkUGCLogicPackSigner::GenerateKeyPair(KeyPair, &Error));

    AkUGCGameModePackLoadTest::FScopedTempFile TempFile(TEXT(".json"));
    TestTrue(*Error, AkUGCGameModePackLoadTest::WriteSignedPackFile(Document, KeyPair, TempFile.Path, &Error));

    UWorld* World = AkUGCGameModePackLoadTest::CreateTestWorld();
    AAkUGCGameMode* GameMode = World->SpawnActor<AAkUGCGameMode>();
    TestNotNull(TEXT("GameMode spawns in test world"), GameMode);
    AAkUGCGameState* GameState = World->SpawnActor<AAkUGCGameState>();
    TestNotNull(TEXT("GameState spawns in test world"), GameState);
    if (!GameMode || !GameState)
    {
        AkUGCGameModePackLoadTest::DestroyTestWorld(World);
        return false;
    }

    TestTrue(*Error, GameMode->LoadAndInitializeAuthoritySession(TempFile.Path, KeyPair.PublicKey, &Error));
    TestTrue(TEXT("Authority session is ready before join"), GameMode->HasAuthoritySession());

    UAkUGCLogicRuntimeSubsystem* Logic = World->GetSubsystem<UAkUGCLogicRuntimeSubsystem>();
    TestNotNull(TEXT("Logic runtime subsystem is available"), Logic);
    if (!Logic)
    {
        AkUGCGameModePackLoadTest::DestroyTestWorld(World);
        return false;
    }

    // 推进 0.5s：第 1 波 3 个敌人已全部刷出、正在沿路径移动（尚未到达终点），模拟新客户端在此刻加入。
    TestTrue(TEXT("Advance into first wave spawning"), Logic->AdvanceLogicTime(0.5).bSucceeded);

    // 权威端把完整玩法状态投影到 GameState（等价于新客户端初始复制的全量快照）。
    GameMode->ProjectStateToGameState(GameState);

    // 新客户端仅凭复制快照恢复的完整状态。
    const FAkUGCWaveRuntimeSnapshot Snapshot = GameState->GetWaveSnapshot();
    TestEqual(TEXT("Join-in-progress restores wave state"),
        Snapshot.State, EAkUGCWaveRuntimeState::WaitingForEnemies);
    TestEqual(TEXT("Join-in-progress restores current wave index"),
        Snapshot.CurrentWaveIndex, 0);
    TestEqual(TEXT("Join-in-progress restores total wave count"),
        Snapshot.TotalWaveCount, 3);
    TestEqual(TEXT("Join-in-progress restores match result"),
        GameState->GetMatchResult(), EAkUGCTowerDefenseMatchResult::InProgress);
    TestEqual(TEXT("Join-in-progress restores base health before any goal reached"),
        GameState->GetBaseHealthCurrent(), 25.0);
    TestEqual(TEXT("Join-in-progress restores all three active enemies"),
        GameState->GetActiveEnemyCount(), 3);

    AkUGCGameModePackLoadTest::DestroyTestWorld(World);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCGameModeJoinInProgressAfterDefeatTest,
    "AkUGC.Runtime.GameMode.JoinInProgressAfterDefeat",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCGameModeJoinInProgressAfterDefeatTest::RunTest(const FString& Parameters)
{
    if (!GEngine)
    {
        AddError(TEXT("Engine is not available."));
        return false;
    }

    FAkUGCProjectDocument Document;
    FString Error;
    TestTrue(*Error, AkUGCPlayableSceneFactory::MakePlayableTowerDefenseDocument(Document, &Error));

    FAkUGCLogicPackKeyPair KeyPair;
    TestTrue(*Error, FAkUGCLogicPackSigner::GenerateKeyPair(KeyPair, &Error));

    AkUGCGameModePackLoadTest::FScopedTempFile TempFile(TEXT(".json"));
    TestTrue(*Error, AkUGCGameModePackLoadTest::WriteSignedPackFile(Document, KeyPair, TempFile.Path, &Error));

    UWorld* World = AkUGCGameModePackLoadTest::CreateTestWorld();
    AAkUGCGameMode* GameMode = World->SpawnActor<AAkUGCGameMode>();
    TestNotNull(TEXT("GameMode spawns in test world"), GameMode);
    AAkUGCGameState* GameState = World->SpawnActor<AAkUGCGameState>();
    TestNotNull(TEXT("GameState spawns in test world"), GameState);
    if (!GameMode || !GameState)
    {
        AkUGCGameModePackLoadTest::DestroyTestWorld(World);
        return false;
    }

    TestTrue(*Error, GameMode->LoadAndInitializeAuthoritySession(TempFile.Path, KeyPair.PublicKey, &Error));

    UAkUGCLogicRuntimeSubsystem* Logic = World->GetSubsystem<UAkUGCLogicRuntimeSubsystem>();
    TestNotNull(TEXT("Logic runtime subsystem is available"), Logic);
    if (!Logic)
    {
        AkUGCGameModePackLoadTest::DestroyTestWorld(World);
        return false;
    }

    // 无塔保护，三敌先后到达终点击穿基地，推进足够长时间抵达 Defeat 终局。
    // 敌人从 spawn point (250,50) 出发、moveSpeed=300、总路径 750，第 3 个敌人在 t=3.0 到达终点；
    // 推进 4.0s 留足余量，确保三敌全部到达、基地归零后进入稳定 Defeat 终局。
    TestTrue(TEXT("Advance to base defeat"), Logic->AdvanceLogicTime(4.0).bSucceeded);

    GameMode->ProjectStateToGameState(GameState);

    // 终局后加入的新客户端仍能恢复完整终局状态。
    TestEqual(TEXT("Join-in-progress restores defeat result"),
        GameState->GetMatchResult(), EAkUGCTowerDefenseMatchResult::Defeat);
    TestEqual(TEXT("Join-in-progress restores base health clamped to zero"),
        GameState->GetBaseHealthCurrent(), 0.0);
    TestEqual(TEXT("Join-in-progress restores wave index at defeat"),
        GameState->GetWaveSnapshot().CurrentWaveIndex, 0);
    TestEqual(TEXT("Join-in-progress restores total wave count at defeat"),
        GameState->GetWaveSnapshot().TotalWaveCount, 3);
    TestEqual(TEXT("Join-in-progress restores no active enemies after defeat"),
        GameState->GetActiveEnemyCount(), 0);

    AkUGCGameModePackLoadTest::DestroyTestWorld(World);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCTowerDefenseFinalStateHashTest,
    "AkUGC.Runtime.GameMode.TowerDefenseFinalStateHash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ServerContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCTowerDefenseFinalStateHashTest::RunTest(const FString& Parameters)
{
    if (!GEngine)
    {
        AddError(TEXT("Engine is not available."));
        return false;
    }

    FAkUGCLogicPackKeyPair KeyPair;
    FString Error;
    TestTrue(*Error, FAkUGCLogicPackSigner::GenerateKeyPair(KeyPair, &Error));

    // 场景 1：标准三波场景（无塔）→ Defeat 终局，验证哈希与推进步长无关。
    FAkUGCProjectDocument DefeatDocument;
    TestTrue(*Error, AkUGCPlayableSceneFactory::MakePlayableTowerDefenseDocument(DefeatDocument, &Error));
    FString DefeatLarge;
    FString DefeatSmall;
    TestTrue(*Error, AkUGCGameModePackLoadTest::RunTerminalHash(DefeatDocument, KeyPair, 4.0, false, DefeatLarge, &Error));
    TestTrue(*Error, AkUGCGameModePackLoadTest::RunTerminalHash(DefeatDocument, KeyPair, 4.0, true, DefeatSmall, &Error));
    TestEqual(TEXT("Defeat terminal hash is delta-independent"), DefeatLarge, DefeatSmall);

    // 场景 2：标准场景 + 强塔 → Victory 终局，同样验证哈希与推进步长无关。
    FAkUGCProjectDocument VictoryDocument;
    TestTrue(*Error, AkUGCGameModePackLoadTest::MakeVictoryDocument(DefeatDocument, VictoryDocument, &Error));
    FString VictoryLarge;
    FString VictorySmall;
    TestTrue(*Error, AkUGCGameModePackLoadTest::RunTerminalHash(VictoryDocument, KeyPair, 3.0, false, VictoryLarge, &Error));
    TestTrue(*Error, AkUGCGameModePackLoadTest::RunTerminalHash(VictoryDocument, KeyPair, 3.0, true, VictorySmall, &Error));
    TestEqual(TEXT("Victory terminal hash is delta-independent"), VictoryLarge, VictorySmall);

    // 两种终局必然不同；日志输出哈希供 Win64 Client / DS 跨 target 对比。
    TestTrue(TEXT("Victory and Defeat produce distinct hashes"), VictoryLarge != DefeatLarge);
    UE_LOG(LogTemp, Display,
        TEXT("[TowerDefenseFinalStateHash] Defeat=%s Victory=%s"),
        *DefeatLarge, *VictoryLarge);

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

#include "Game/AkUGCGameMode.h"

#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Command/AkUGCCommandExecutor.h"
#include "Document/AkUGCDocument.h"
#include "Game/AkUGCGameState.h"
#include "Game/AkUGCHUD.h"
#include "Pack/AkUGCLogicPack.h"
#include "Pack/AkUGCLogicPackLoader.h"
#include "Prefab/AkUGCOfficialPrefabCatalog.h"
#include "Prefab/AkUGCPrefabRegistry.h"
#include "Scene/AkUGCSceneRuntime.h"
#include "Session/AkUGCDocumentRuntimeSession.h"
#include "Game/AkUGCPlayerController.h"
#include "Template/AkUGCTowerDefenseBot.h"
#include "Template/AkUGCTowerDefenseTemplate.h"
#include "Validation/AkUGCDocumentValidator.h"

AAkUGCGameMode::AAkUGCGameMode()
{
    GameStateClass = AAkUGCGameState::StaticClass();
    HUDClass = AAkUGCHUD::StaticClass();
    PlayerControllerClass = AAkUGCPlayerController::StaticClass();
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.bStartWithTickEnabled = true;
}

AAkUGCGameMode::~AAkUGCGameMode() = default;

void AAkUGCGameMode::BeginPlay()
{
    Super::BeginPlay();
    FString PackPath;
    if (!IsRunningDedicatedServer() && GetNetMode() == NM_Standalone && !Session
        && !FParse::Value(FCommandLine::Get(), TEXT("LogicPack="), PackPath))
    {
        FString Error;
        if (!PrepareDefaultSoloProject(&Error)) { UE_LOG(LogTemp, Error, TEXT("Solo preparation failed: %s"), *Error); }
    }
}

bool AAkUGCGameMode::PrepareDefaultSoloProject(FString* OutError)
{
    FAkUGCPrefabRegistry Registry;
    FAkUGCProjectDocument Document;
    if (!FAkUGCOfficialPrefabCatalog::RegisterTowerDefense(Registry, OutError)
        || !FAkUGCTowerDefenseTemplate::CreateDefaultDocument(Registry, Document, OutError)) { return false; }
    FAkUGCSceneDocument& Scene = Document.Scenes[0];
    Scene.Entities.RemoveAll([](const FAkUGCEntityRecord& Entity) { return Entity.PrefabId == TEXT("official.tower.basic"); });
    for (int32 Index = 0; Index < 2; ++Index)
    {
        FAkUGCEntityRecord Slot;
        if (!Registry.CreateEntityRecord(TEXT("official.gameplay.tower_slot"), FGuid(0xB0700001 + Index, 0, 0, 0),
            FTransform(FVector(Index == 0 ? 650.0 : 1100.0, Index == 0 ? 150.0 : -150.0, 0.0)), Slot, OutError)) { return false; }
        Scene.Entities.Add(Slot);
    }
    return PrepareSoloProject(Document, OutError);
}

bool AAkUGCGameMode::PrepareSoloProject(const FAkUGCProjectDocument& Document, FString* OutError)
{
    if (!HasAuthority() || GetNetMode() == NM_Client || GetNetMode() == NM_DedicatedServer || Session)
    {
        if (OutError) { *OutError = TEXT("Solo preparation requires an idle local authoritative world."); }
        return false;
    }
    FAkUGCPrefabRegistry Registry;
    FAkUGCTowerDefenseParameters Parameters;
    if (Document.Scenes.Num() != 1 || !FAkUGCOfficialPrefabCatalog::RegisterTowerDefense(Registry, OutError)
        || !FAkUGCTowerDefenseTemplate::ReadParameters(Document, Document.Scenes[0].SceneId, Parameters, OutError)
        || !FAkUGCTowerDefenseTemplate::ValidateDocumentBudgets(Document, Registry, OutError)) { return false; }
    TUniquePtr<FAkUGCSceneRuntime> Preview = MakeUnique<FAkUGCSceneRuntime>(GetWorld());
    if (!Preview->LoadScene(Document.Scenes[0], Registry, OutError)) { return false; }
    SceneRuntime.Reset();
    SceneRuntime = MoveTemp(Preview);
    PreparedDocument = Document;
    BotTowerCount = 0;
    if (AAkUGCGameState* State = GetWorld()->GetGameState<AAkUGCGameState>())
    {
        State->ResetForPreparation();
        State->ProjectBaseHealth(Parameters.BaseMaxHealth, Parameters.BaseMaxHealth);
        State->InitializeResources(Parameters.InitialResources);
    }
    return true;
}

bool AAkUGCGameMode::StartSoloMatch(FString* OutError)
{
    AAkUGCGameState* State = GetWorld()->GetGameState<AAkUGCGameState>();
    if (!HasAuthority() || GetNetMode() == NM_Client || GetNetMode() == NM_DedicatedServer || Session
        || !State || State->GetMatchFlow() != EAkUGCMatchFlow::Preparation || PreparedDocument.Scenes.Num() != 1)
    {
        if (OutError) { *OutError = TEXT("Start requires a prepared local match."); }
        return false;
    }
    FAkUGCPrefabRegistry Registry;
    FAkUGCCommandTransaction Transaction;
    int32 Count = 0;
    if (!FAkUGCOfficialPrefabCatalog::RegisterTowerDefense(Registry, OutError)
        || !FAkUGCTowerDefenseBot::BuildPreparationTransaction(PreparedDocument, Registry, Transaction, Count, OutError)) { return false; }
    FAkUGCProjectDocument Candidate = PreparedDocument;
    if (!FAkUGCCommandExecutor::Apply(Candidate, Transaction).bSucceeded) { return false; }
    SceneRuntime.Reset();
    State->ResetForPreparation();
    bHasProjectedState = false;
    if (!InitializeAuthoritySession(Candidate, OutError))
    {
        FString RestoreError;
        PrepareSoloProject(PreparedDocument, &RestoreError);
        return false;
    }
    BotTowerCount = Count;
    bSoloSession = true;
    State->ProjectMatchFlow(EAkUGCMatchFlow::Playing);
    return true;
}

bool AAkUGCGameMode::ReturnToPreparation(FString* OutError)
{
    if (!HasAuthority() || !HasPreparedProject() || (Session && !bSoloSession)
        || GetNetMode() == NM_Client || GetNetMode() == NM_DedicatedServer)
    {
        if (OutError) { *OutError = TEXT("Return requires a local solo project."); }
        return false;
    }
    Session.Reset();
    SceneRuntime.Reset();
    PrefabRegistry.Reset();
    ScoreAccumulator.Initialize(0);
    bHasProjectedState = false;
    return PrepareSoloProject(PreparedDocument, OutError);
}

bool AAkUGCGameMode::InitializeAuthoritySession(FAkUGCProjectDocument& Document, FString* OutError)
{
    if (Session.IsValid())
    {
        if (OutError)
        {
            *OutError = TEXT("Authority session is already initialized.");
        }
        return false;
    }

    UWorld* World = GetWorld();
    if (!World)
    {
        if (OutError)
        {
            *OutError = TEXT("GameMode has no world.");
        }
        return false;
    }

    if (Document.Scenes.IsEmpty())
    {
        if (OutError)
        {
            *OutError = TEXT("Document contains no scene.");
        }
        return false;
    }

    const FAkUGCSceneDocument& Scene = Document.Scenes[0];

    SceneRuntime = MakeUnique<FAkUGCSceneRuntime>(World);
    PrefabRegistry = MakeUnique<FAkUGCPrefabRegistry>();
    if (!FAkUGCOfficialPrefabCatalog::RegisterTowerDefense(*PrefabRegistry, OutError))
    {
        SceneRuntime.Reset();
        PrefabRegistry.Reset();
        return false;
    }

    Session = MakeUnique<FAkUGCDocumentRuntimeSession>(
        *SceneRuntime,
        *PrefabRegistry,
        Scene.SceneId,
        EAkUGCRuntimeSessionMode::PlayAuthority);

    const FAkUGCCommandExecutionResult Result = Session->Initialize(Document);
    if (!Result.bSucceeded)
    {
        if (OutError)
        {
            *OutError = Result.ErrorMessage;
        }
        Session.Reset();
        PrefabRegistry.Reset();
        SceneRuntime.Reset();
        return false;
    }

    bSoloSession = false;
    ScoreAccumulator.Initialize(Scene.Ruleset.InitialResources);
    ProjectStateToGameState(GetWorld()->GetGameState<AAkUGCGameState>());
    return true;
}

bool AAkUGCGameMode::LoadAndInitializeAuthoritySession(
    const FString& PackFilePath,
    const FString& TrustedPublicKeyHex,
    FString* OutError)
{
    if (Session.IsValid())
    {
        if (OutError)
        {
            *OutError = TEXT("Authority session is already initialized.");
        }
        return false;
    }

    const FAkUGCLogicPackLoadResult LoadResult =
        FAkUGCLogicPackLoader::LoadVerifiedFromFile(PackFilePath, TrustedPublicKeyHex);
    if (!LoadResult.bSucceeded)
    {
        if (OutError)
        {
            *OutError = LoadResult.ErrorMessage;
        }
        return false;
    }

    FAkUGCProjectDocument Document = LoadResult.Pack.Document;
    return InitializeAuthoritySession(Document, OutError);
}

bool AAkUGCGameMode::HasAuthoritySession() const
{
    return Session.IsValid();
}

void AAkUGCGameMode::InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage)
{
    Super::InitGame(MapName, Options, ErrorMessage);

    // 服务器权威端启动时，从命令行解析并自动加载签名 Logic Pack：
    //   -LogicPack=<path>  发布包 JSON 文件路径
    //   -LogicPackKey=<hex>  可信公钥（十六进制）
    FString LogicPackPath;
    if (FParse::Value(FCommandLine::Get(), TEXT("LogicPack="), LogicPackPath))
    {
        FString LogicPackKey;
        FParse::Value(FCommandLine::Get(), TEXT("LogicPackKey="), LogicPackKey);

        FString LoadError;
        if (!LoadAndInitializeAuthoritySession(LogicPackPath, LogicPackKey, &LoadError))
        {
            UE_LOG(LogTemp, Error,
                TEXT("[AkUGC] Authority Logic Pack load failed for '%s': %s"),
                *LogicPackPath, *LoadError);
        }
        else
        {
            UE_LOG(LogTemp, Log,
                TEXT("[AkUGC] Authority Logic Pack loaded: %s"), *LogicPackPath);
        }
    }
}

void AAkUGCGameMode::ProjectStateToGameState(AAkUGCGameState* InGameState)
{
    if (!HasAuthority() || !InGameState || !Session.IsValid() || !SceneRuntime.IsValid())
    {
        return;
    }

    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    UAkUGCLogicRuntimeSubsystem* Logic = World->GetSubsystem<UAkUGCLogicRuntimeSubsystem>();
    if (!Logic)
    {
        return;
    }

    const FAkUGCWaveRuntimeSnapshot Snapshot = Logic->GetWaveRuntimeState();

    // 忽略逐帧变化的瞬态字段（SecondsUntilNextBoundary），仅在离散状态变化时投影波次快照。
    const bool bWaveChanged = !bHasProjectedState
        || Snapshot.State != LastProjectedWaveSnapshot.State
        || Snapshot.Result != LastProjectedWaveSnapshot.Result
        || Snapshot.CurrentWaveIndex != LastProjectedWaveSnapshot.CurrentWaveIndex
        || Snapshot.CurrentWaveId != LastProjectedWaveSnapshot.CurrentWaveId
        || Snapshot.TotalWaveCount != LastProjectedWaveSnapshot.TotalWaveCount;
    if (bWaveChanged)
    {
        InGameState->ProjectWaveSnapshot(Snapshot);
        LastProjectedWaveSnapshot = Snapshot;
    }

    const double BaseCurrent = SceneRuntime->GetBaseCurrentHealth();
    const double BaseMaximum = SceneRuntime->GetBaseMaximumHealth();
    if (!bHasProjectedState
        || BaseCurrent != LastProjectedBaseHealthCurrent
        || BaseMaximum != LastProjectedBaseHealthMaximum)
    {
        InGameState->ProjectBaseHealth(BaseCurrent, BaseMaximum);
        LastProjectedBaseHealthCurrent = BaseCurrent;
        LastProjectedBaseHealthMaximum = BaseMaximum;
    }

    const int32 EnemyCount = SceneRuntime->GetActiveEnemyMovementCount();
    if (!bHasProjectedState || EnemyCount != LastProjectedActiveEnemyCount)
    {
        InGameState->ProjectActiveEnemyCount(EnemyCount);
        LastProjectedActiveEnemyCount = EnemyCount;
    }

    ScoreAccumulator.Update(Logic->GetSpawnedEntities(), Logic->GetDeathEvents(), Snapshot.Result, BaseCurrent, BaseMaximum);
    InGameState->ProjectScoreSnapshot(ScoreAccumulator.GetSnapshot());
    InGameState->ProjectMatchFlow(ScoreAccumulator.GetSnapshot().bSettled ? EAkUGCMatchFlow::Settlement : EAkUGCMatchFlow::Playing);
    bHasProjectedState = true;
}

void AAkUGCGameMode::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);

    if (Session.IsValid())
    {
        ProjectStateToGameState(GetWorld()->GetGameState<AAkUGCGameState>());
    }
}

void AAkUGCGameMode::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    Session.Reset();
    PrefabRegistry.Reset();
    SceneRuntime.Reset();

    Super::EndPlay(EndPlayReason);
}

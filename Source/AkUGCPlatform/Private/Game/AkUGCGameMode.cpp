#include "Game/AkUGCGameMode.h"

#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Command/AkUGCCommandExecutor.h"
#include "Document/AkUGCDocument.h"
#include "Game/AkUGCGameState.h"
#include "Pack/AkUGCLogicPack.h"
#include "Pack/AkUGCLogicPackLoader.h"
#include "Prefab/AkUGCOfficialPrefabCatalog.h"
#include "Prefab/AkUGCPrefabRegistry.h"
#include "Scene/AkUGCSceneRuntime.h"
#include "Session/AkUGCDocumentRuntimeSession.h"

AAkUGCGameMode::AAkUGCGameMode()
{
    GameStateClass = AAkUGCGameState::StaticClass();
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.bStartWithTickEnabled = true;
}

AAkUGCGameMode::~AAkUGCGameMode() = default;

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
    if (!InGameState || !Session.IsValid() || !SceneRuntime.IsValid())
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

    bHasProjectedState = true;
}

void AAkUGCGameMode::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);

    if (Session.IsValid())
    {
        ProjectStateToGameState(GetGameState<AAkUGCGameState>());
    }
}

void AAkUGCGameMode::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    Session.Reset();
    PrefabRegistry.Reset();
    SceneRuntime.Reset();

    Super::EndPlay(EndPlayReason);
}

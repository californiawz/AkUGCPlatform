#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "Subsystem/AkUGCLogicRuntimeSubsystem.h"
#include "Prefab/AkUGCPrefabRegistry.h"
#include "Scene/AkUGCSceneRuntime.h"
#include "Gameplay/AkUGCTowerDefenseScore.h"
#include "Session/AkUGCDocumentRuntimeSession.h"
#include "Game/AkUGCRoom.h"
#include "AkUGCGameMode.generated.h"

class AAkUGCGameState;
struct FAkUGCProjectDocument;

/**
 * 塔防权威会话的托管点。
 *
 * 服务器权威端通过 InitializeAuthoritySession 托管 PlayAuthority 会话，
 * 并在 Tick 中把 Scene Runtime 与 Logic Runtime 的可观察状态投影到
 * GameState 的复制属性；客户端仅观察复制状态，不执行权威玩法。
 *
 * Document 来源（Logic Pack）由 P8 接入，当前由调用方注入。
 */
UCLASS()
class AKUGCPLATFORM_API AAkUGCGameMode : public AGameModeBase
{
    GENERATED_BODY()

public:
    AAkUGCGameMode();
    ~AAkUGCGameMode() override;

    // 服务器权威端初始化三波塔防会话。重复调用会被拒绝。
    bool InitializeAuthoritySession(FAkUGCProjectDocument& Document, FString* OutError = nullptr);

    // 服务器权威端从磁盘加载签名 Logic Pack 并初始化三波塔防会话。
    // 未签名、签名不合法、文件缺失或内容非法都会被拒绝。
    bool LoadAndInitializeAuthoritySession(
        const FString& PackFilePath,
        const FString& TrustedPublicKeyHex,
        FString* OutError = nullptr);

    bool HasAuthoritySession() const;
    bool PrepareDefaultSoloProject(FString* OutError = nullptr);
    bool PrepareSoloProject(const FAkUGCProjectDocument& Document, FString* OutError = nullptr);
    bool StartSoloMatch(FString* OutError = nullptr);
    bool PrepareLibraryWork(const FGuid& ReleaseId, const FString& LibraryDirectory, FString* OutError = nullptr);
    bool ReturnToPreparation(FString* OutError = nullptr);
    const FAkUGCProjectDocument& GetPreparedDocument() const { return PreparedDocument; }
    int32 GetBotTowerCount() const { return BotTowerCount; }
    bool HasPreparedProject() const { return !PreparedDocument.Scenes.IsEmpty(); }
    virtual void BeginPlay() override;
    bool IsRoomMode() const { return bRoomMode; }
    bool SetMemberReady(APlayerController* Player, bool bReady);
    bool StartRoomMatch(APlayerController* Player);
    bool ReturnRoomToPreparation(APlayerController* Player);
    bool PrepareRoomProject(const FAkUGCProjectDocument& Document, FString* OutError = nullptr);
    virtual void PreLogin(const FString& Options, const FString& Address, const FUniqueNetIdRepl& UniqueId, FString& ErrorMessage) override;
    virtual void PostLogin(APlayerController* NewPlayer) override;
    virtual void Logout(AController* Exiting) override;

    virtual void InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage) override;

    // 把权威玩法状态投影到 GameState（仅可观察状态变化时触发复制）。
    void ProjectStateToGameState(AAkUGCGameState* GameState);

    virtual void Tick(float DeltaSeconds) override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
    TUniquePtr<FAkUGCSceneRuntime> SceneRuntime;
    TUniquePtr<FAkUGCPrefabRegistry> PrefabRegistry;
    TUniquePtr<FAkUGCDocumentRuntimeSession> Session;

    // 变化检测缓存，避免逐帧复制瞬态值（如 SecondsUntilNextBoundary）。
    FAkUGCWaveRuntimeSnapshot LastProjectedWaveSnapshot;
    double LastProjectedBaseHealthCurrent = 0.0;
    double LastProjectedBaseHealthMaximum = 0.0;
    int32 LastProjectedActiveEnemyCount = 0;
    bool bHasProjectedState = false;
    FAkUGCTowerDefenseScoreAccumulator ScoreAccumulator;
    FAkUGCProjectDocument PreparedDocument;
    int32 BotTowerCount = 0;
    bool bSoloSession = false;
    bool bRoomMode = false;
    bool bRoomSession = false;
    FAkUGCRoomRoster RoomRoster;
    TMap<TWeakObjectPtr<APlayerController>, int32> RoomPlayerIds;
    void PublishRoom();
    bool PrepareProject(const FAkUGCProjectDocument& Document, FString* OutError);
    bool StartPreparedMatch(FString* OutError);
    bool InitializeSessionInternal(FAkUGCProjectDocument& Document, FString* OutError);
    bool bRoomLoadFailed = false;
    bool bLocalOverride = false;
};

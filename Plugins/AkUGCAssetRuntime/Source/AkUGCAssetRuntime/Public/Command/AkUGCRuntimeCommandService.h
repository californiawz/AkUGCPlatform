#pragma once

#include "Command/AkUGCCommandExecutor.h"
#include "Document/AkUGCDocument.h"

class FAkUGCDocumentRuntimeSession;
class FAkUGCPrefabRegistry;

enum class EAkUGCEditingClient : uint8
{
    Desktop,
    Mobile
};

class AKUGCASSETRUNTIME_API FAkUGCRuntimeCommandService
{
public:
    FAkUGCRuntimeCommandService(
        FAkUGCProjectDocument& InDocument,
        FAkUGCDocumentRuntimeSession& InSession,
        const FAkUGCPrefabRegistry& InRegistry,
        FGuid InSceneId,
        EAkUGCEditingClient InEditingClient = EAkUGCEditingClient::Desktop);

    FAkUGCCommandExecutionResult PlacePrefab(
        FName PrefabId,
        const FTransform& Transform,
        FGuid& OutEntityId);
    FAkUGCCommandExecutionResult DeleteEntities(
        const TSet<FGuid>& EntityIds,
        const FString& Label = TEXT("Delete entities"));
    FAkUGCCommandExecutionResult DuplicateEntity(
        const FGuid& SourceEntityId,
        const FVector& WorldOffset,
        FGuid& OutEntityId);
    FAkUGCCommandExecutionResult SetEntityTransform(
        const FGuid& EntityId,
        const FTransform& Transform);
    FAkUGCCommandExecutionResult SetEntityTransforms(
        const TMap<FGuid, FTransform>& Transforms,
        const FString& Label);
    FAkUGCCommandExecutionResult SetEntityParent(
        const FGuid& EntityId,
        const FGuid& ParentEntityId);
    FAkUGCCommandExecutionResult SetEntityParentAndTransform(
        const FGuid& EntityId,
        const FGuid& ParentEntityId,
        const TOptional<FTransform>& WorldTransform,
        const FString& Label);
    FAkUGCCommandExecutionResult SetEntityProperty(
        const FGuid& EntityId,
        FName ComponentTypeId,
        FName PropertyId,
        const FAkUGCValue& Value);
    FAkUGCCommandExecutionResult AddWave(const FAkUGCTowerDefenseWave& Wave, int32 WaveIndex = INDEX_NONE);
    FAkUGCCommandExecutionResult UpdateWave(const FAkUGCTowerDefenseWave& Wave);
    FAkUGCCommandExecutionResult DeleteWave(const FGuid& WaveId);
    FAkUGCCommandExecutionResult MoveWave(const FGuid& WaveId, int32 TargetWaveIndex);
    FAkUGCCommandExecutionResult SetRulesetSettings(
        double WaveIntervalSeconds,
        EAkUGCTowerDefenseDefeatCondition DefeatCondition,
        EAkUGCTowerDefenseVictoryCondition VictoryCondition);

    FAkUGCCommandExecutionResult Undo();
    FAkUGCCommandExecutionResult Redo();
    bool CanUndo() const;
    bool CanRedo() const;

    const FAkUGCEntityRecord* FindEntity(const FGuid& EntityId) const;
    const FAkUGCSceneDocument* FindScene() const;

private:
    FAkUGCCommandExecutionResult Execute(FAkUGCCommandTransaction&& Transaction);

    FAkUGCProjectDocument& Document;
    FAkUGCDocumentRuntimeSession& Session;
    const FAkUGCPrefabRegistry& Registry;
    FGuid SceneId;
    EAkUGCEditingClient EditingClient;
};

#include "Scene/AkUGCSandboxSceneHost.h"

#include "Gameplay/AkUGCTowerDefenseMovement.h"
#include "Scene/AkUGCSceneRuntime.h"
#include "Subsystem/AkUGCLogicRuntimeSubsystem.h"

FAkUGCSandboxSceneHost::FAkUGCSandboxSceneHost(FAkUGCSceneRuntime& InSceneRuntime, const FAkUGCPrefabRegistry& InRegistry)
	: SceneRuntime(InSceneRuntime)
	, Registry(InRegistry)
{
}

void FAkUGCSandboxSceneHost::EmitMessage(const FString& Message)
{
	Messages.Add(Message);
}

bool FAkUGCSandboxSceneHost::QueryEntityHealth(const FString& EntityId, double& OutCurrent, double& OutMaximum)
{
	FGuid EntityGuid;
	if (!FGuid::Parse(EntityId, EntityGuid))
	{
		return false;
	}

	FAkUGCRuntimeHealth Health;
	if (!SceneRuntime.GetRuntimeHealth(EntityGuid, Health))
	{
		return false;
	}

	OutCurrent = Health.Current;
	OutMaximum = Health.Maximum;
	return true;
}

bool FAkUGCSandboxSceneHost::ApplyDamage(
	const FString& SourceEntityId,
	const FString& TargetEntityId,
	double Damage,
	double& OutAppliedDamage,
	double& OutHealthAfter,
	bool& OutKilled,
	FString& OutError)
{
	FGuid SourceGuid;
	FGuid TargetGuid;
	if (!FGuid::Parse(SourceEntityId, SourceGuid))
	{
		OutError = TEXT("Invalid source entity id.");
		return false;
	}
	if (!FGuid::Parse(TargetEntityId, TargetGuid))
	{
		OutError = TEXT("Invalid target entity id.");
		return false;
	}

	FAkUGCRuntimeDamage DamageResult;
	if (!SceneRuntime.ApplyRuntimeDamage(SourceGuid, TargetGuid, Damage, DamageResult, OutError))
	{
		return false;
	}

	OutAppliedDamage = DamageResult.AppliedDamage;
	OutHealthAfter = DamageResult.HealthAfterDamage;
	OutKilled = DamageResult.bKilled;
	return true;
}

bool FAkUGCSandboxSceneHost::SpawnEntity(
	const FString& PrefabId,
	const FString& AnchorEntityId,
	FString& OutEntityId,
	FString& OutError)
{
	FGuid AnchorGuid;
	if (!AnchorEntityId.IsEmpty() && !FGuid::Parse(AnchorEntityId, AnchorGuid))
	{
		OutError = TEXT("Invalid anchor entity id.");
		return false;
	}

	FGuid SpawnedGuid;
	if (!SceneRuntime.SpawnSandboxEntity(FName(*PrefabId), AnchorGuid, Registry, SpawnedGuid, OutError))
	{
		return false;
	}

	OutEntityId = SpawnedGuid.ToString();
	return true;
}

bool FAkUGCSandboxSceneHost::QueryWaveState(FAkUGCSandboxWaveState& OutState)
{
	FAkUGCWaveRuntimeSnapshot Snapshot;
	if (!SceneRuntime.GetWaveRuntimeState(Snapshot))
	{
		return false;
	}

	OutState.WaveIndex = Snapshot.CurrentWaveIndex;
	OutState.TotalWaves = Snapshot.TotalWaveCount;
	OutState.SecondsUntilNextBoundary = Snapshot.SecondsUntilNextBoundary;
	OutState.State = static_cast<int32>(Snapshot.State);
	OutState.Result = static_cast<int32>(Snapshot.Result);
	return true;
}

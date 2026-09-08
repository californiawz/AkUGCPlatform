#include "Scene/AkUGCSandboxSceneHost.h"

#include "Gameplay/AkUGCTowerDefenseMovement.h"
#include "Scene/AkUGCSceneRuntime.h"

FAkUGCSandboxSceneHost::FAkUGCSandboxSceneHost(FAkUGCSceneRuntime& InSceneRuntime)
	: SceneRuntime(InSceneRuntime)
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

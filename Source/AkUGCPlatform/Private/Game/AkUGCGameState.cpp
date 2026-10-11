#include "Game/AkUGCGameState.h"

#include "Net/UnrealNetwork.h"

AAkUGCGameState::AAkUGCGameState()
{
}

void AAkUGCGameState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);

    DOREPLIFETIME(AAkUGCGameState, ReplicatedWaveSnapshot);
    DOREPLIFETIME(AAkUGCGameState, ReplicatedBaseHealthCurrent);
    DOREPLIFETIME(AAkUGCGameState, ReplicatedBaseHealthMaximum);
    DOREPLIFETIME(AAkUGCGameState, ReplicatedActiveEnemyCount);
    DOREPLIFETIME(AAkUGCGameState, ReplicatedScoreSnapshot);
}

void AAkUGCGameState::ProjectWaveSnapshot(const FAkUGCWaveRuntimeSnapshot& Snapshot)
{
    if (HasAuthority()) { ReplicatedWaveSnapshot = Snapshot; }
}

void AAkUGCGameState::ProjectBaseHealth(double Current, double Maximum)
{
    if (HasAuthority() && FMath::IsFinite(Current) && FMath::IsFinite(Maximum) && Current >= 0.0 && Maximum >= Current)
    {
        ReplicatedBaseHealthCurrent = Current;
        ReplicatedBaseHealthMaximum = Maximum;
    }
}

void AAkUGCGameState::ProjectActiveEnemyCount(int32 Count)
{
    if (HasAuthority() && Count >= 0) { ReplicatedActiveEnemyCount = Count; }
}

void AAkUGCGameState::InitializeResources(int32 InitialResources)
{
    if (!HasAuthority() || bResourcesInitialized || InitialResources < 0
        || InitialResources > AkUGCTowerDefenseRulesetLimits::MaxInitialResources)
    {
        return;
    }
    ReplicatedScoreSnapshot.Resources = InitialResources;
    bResourcesInitialized = true;
}

bool AAkUGCGameState::ProjectScoreSnapshot(const FAkUGCTowerDefenseScoreSnapshot& Snapshot)
{
    if (!HasAuthority() || ReplicatedScoreSnapshot.bSettled
        || Snapshot.TeamScore < 0 || Snapshot.TeamScore > AkUGCScoreRules::MaxScore
        || Snapshot.Resources < 0 || Snapshot.Resources > AkUGCScoreRules::MaxResources
        || Snapshot.EnemyKills < 0 || Snapshot.EnemyKills > AkUGCTowerDefenseRulesetLimits::MaxTotalEnemyCount
        || !FMath::IsFinite(Snapshot.FinalBaseCurrentHealth) || !FMath::IsFinite(Snapshot.FinalBaseMaximumHealth)
        || Snapshot.FinalBaseCurrentHealth < 0.0 || Snapshot.FinalBaseMaximumHealth < Snapshot.FinalBaseCurrentHealth
        || (Snapshot.Result != EAkUGCTowerDefenseMatchResult::InProgress
            && Snapshot.Result != EAkUGCTowerDefenseMatchResult::Victory && Snapshot.Result != EAkUGCTowerDefenseMatchResult::Defeat)
        || (Snapshot.bSettled && (Snapshot.Result == EAkUGCTowerDefenseMatchResult::InProgress || Snapshot.FinalBaseMaximumHealth <= 0.0))
        || (!Snapshot.bSettled && Snapshot.Result != EAkUGCTowerDefenseMatchResult::InProgress))
    {
        return false;
    }
    ReplicatedScoreSnapshot = Snapshot;
    bResourcesInitialized = true;
    OnRep_ScoreSnapshot();
    return true;
}

void AAkUGCGameState::OnRep_ScoreSnapshot()
{
    OnScoreSnapshotReplicated.Broadcast();
}

int32 AAkUGCGameState::GetResources() const
{
    return ReplicatedScoreSnapshot.Resources;
}

FAkUGCWaveRuntimeSnapshot AAkUGCGameState::GetWaveSnapshot() const
{
    return ReplicatedWaveSnapshot;
}

EAkUGCTowerDefenseMatchResult AAkUGCGameState::GetMatchResult() const
{
    return ReplicatedWaveSnapshot.Result;
}

double AAkUGCGameState::GetBaseHealthCurrent() const
{
    return ReplicatedBaseHealthCurrent;
}

double AAkUGCGameState::GetBaseHealthMaximum() const
{
    return ReplicatedBaseHealthMaximum;
}

int32 AAkUGCGameState::GetActiveEnemyCount() const
{
    return ReplicatedActiveEnemyCount;
}

void AAkUGCGameState::OnRep_WaveSnapshot()
{
    OnWaveSnapshotReplicated.Broadcast();

    if (!bMatchEndedBroadcast && ReplicatedWaveSnapshot.Result != EAkUGCTowerDefenseMatchResult::InProgress)
    {
        bMatchEndedBroadcast = true;
        OnMatchEndedReplicated.Broadcast(ReplicatedWaveSnapshot.Result);
    }
}

void AAkUGCGameState::OnRep_BaseHealth()
{
    OnBaseHealthReplicated.Broadcast();
}

void AAkUGCGameState::OnRep_ActiveEnemyCount()
{
    OnActiveEnemyCountReplicated.Broadcast();
}

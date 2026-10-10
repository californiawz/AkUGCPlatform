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
    DOREPLIFETIME(AAkUGCGameState, ReplicatedResources);
}

void AAkUGCGameState::ProjectWaveSnapshot(const FAkUGCWaveRuntimeSnapshot& Snapshot)
{
    ReplicatedWaveSnapshot = Snapshot;
}

void AAkUGCGameState::ProjectBaseHealth(double Current, double Maximum)
{
    ReplicatedBaseHealthCurrent = Current;
    ReplicatedBaseHealthMaximum = Maximum;
}

void AAkUGCGameState::ProjectActiveEnemyCount(int32 Count)
{
    ReplicatedActiveEnemyCount = Count;
}

void AAkUGCGameState::InitializeResources(int32 InitialResources)
{
    if (!HasAuthority() || bResourcesInitialized || InitialResources < 0
        || InitialResources > AkUGCTowerDefenseRulesetLimits::MaxInitialResources)
    {
        return;
    }
    ReplicatedResources = InitialResources;
    bResourcesInitialized = true;
}

int32 AAkUGCGameState::GetResources() const
{
    return ReplicatedResources;
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

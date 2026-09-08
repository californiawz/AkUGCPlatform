#include "Scene/AkUGCSceneRuntime.h"

#include "Command/AkUGCCommand.h"
#include "Components/StaticMeshComponent.h"
#include "Document/AkUGCDocument.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Entity/AkUGCEntityBindingComponent.h"
#include "Entity/AkUGCRuntimeEntityActor.h"
#include "Gameplay/AkUGCTowerDefensePath.h"
#include "Logic/AkUGCLogicRunner.h"
#include "Prefab/AkUGCPrefabRegistry.h"
#include "Subsystem/AkUGCLogicRuntimeSubsystem.h"

namespace
{
    bool Fail(FString* OutError, const FString& Message)
    {
        if (OutError)
        {
            *OutError = Message;
        }
        return false;
    }

    UAkUGCEntityBindingComponent* FindBinding(AActor* Actor)
    {
        return Actor ? Actor->FindComponentByClass<UAkUGCEntityBindingComponent>() : nullptr;
    }
}

FAkUGCSceneRuntime::FAkUGCSceneRuntime(UWorld* InWorld)
    : World(InWorld)
{
}

FAkUGCSceneRuntime::~FAkUGCSceneRuntime()
{
    *LifetimeToken = false;
    Unload();
}

bool FAkUGCSceneRuntime::LoadScene(
    const FAkUGCSceneDocument& Scene,
    const FAkUGCPrefabRegistry& Registry,
    FString* OutError)
{
    if (!ValidateScene(Scene, Registry, OutError))
    {
        return false;
    }
    const FAkUGCTowerDefensePathBuildResult PathResult = FAkUGCTowerDefensePathBuilder::Build(Scene);
    if (!PathResult.bSucceeded)
    {
        return Fail(OutError, FString::Printf(TEXT("%s: %s"), *PathResult.ErrorPath, *PathResult.ErrorMessage));
    }

    Unload();
    ActiveSceneId = Scene.SceneId;
    TowerDefensePath = PathResult.Path;

    for (const FAkUGCEntityRecord& Entity : Scene.Entities)
    {
        if (!SpawnEntity(Entity, Registry, OutError))
        {
            Unload();
            return false;
        }
    }

    if (!AttachParents(Scene, OutError))
    {
        Unload();
        return false;
    }

    return true;
}

bool FAkUGCSceneRuntime::RunGameStartLogic(
    const FAkUGCSceneDocument& Scene,
    const FAkUGCPrefabRegistry& Registry,
    const TWeakPtr<bool, ESPMode::ThreadSafe>& SessionLifetime,
    const FGuid& ExecutionOwnerId,
    bool bRequireAuthority,
    FString* OutError)
{
    UWorld* RuntimeWorld = World.Get();
    if (!RuntimeWorld)
    {
        return Fail(OutError, TEXT("Logic execution requires a valid runtime world."));
    }
    if (RuntimeWorld->WorldType != EWorldType::Game
        && RuntimeWorld->WorldType != EWorldType::PIE
        && RuntimeWorld->WorldType != EWorldType::GamePreview)
    {
        return Fail(OutError, TEXT("Logic execution requires a Game, PIE, or GamePreview world."));
    }
    if (bRequireAuthority && RuntimeWorld->GetNetMode() == NM_Client)
    {
        return Fail(OutError, TEXT("PlayAuthority session cannot execute in a client world."));
    }
    if (!bRequireAuthority && RuntimeWorld->GetNetMode() == NM_DedicatedServer)
    {
        return Fail(OutError, TEXT("Preview session cannot execute in a dedicated server world."));
    }

    for (const FAkUGCLogicNode& Node : Scene.LogicGraph.Nodes)
    {
        if (Node.Type == EAkUGCLogicNodeType::Spawn && !Registry.Find(Node.SpawnPrefabId))
        {
            return Fail(OutError, FString::Printf(
                TEXT("Spawn Prefab '%s' is not registered."),
                *Node.SpawnPrefabId.ToString()));
        }
    }

    UAkUGCLogicRuntimeSubsystem* LogicRuntime = RuntimeWorld
        ? RuntimeWorld->GetSubsystem<UAkUGCLogicRuntimeSubsystem>()
        : nullptr;
    if (!LogicRuntime)
    {
        return Fail(OutError, TEXT("Logic Runtime subsystem is not available for the scene world."));
    }
    const TWeakPtr<bool, ESPMode::ThreadSafe> WeakLifetime = LifetimeToken;
    FString HandlerError;
    if (!LogicRuntime->SetRuntimeHandlers(
        ExecutionOwnerId,
        [this, &Registry](
            const FAkUGCLogicSpawnEffect& SpawnEffect,
            FAkUGCLogicSpawnPlan& OutPlan,
            FString& OutPlanError)
        {
            return BuildLogicSpawnPlan(SpawnEffect, Registry, OutPlan, OutPlanError);
        },
        [this, &Registry](
            const FAkUGCLogicSpawnEffect& SpawnEffect,
            FGuid& OutEntityId,
            FString& OutSpawnError)
        {
            return SpawnLogicPrefab(SpawnEffect, Registry, OutEntityId, OutSpawnError);
        },
        [this](
            double DeltaSeconds,
            double& OutAdvancedSeconds,
            bool& OutProcessedBoundary,
            FAkUGCTowerDefenseGameplayEvents& OutEvents,
            FString& OutMovementError)
        {
            return AdvanceTowerDefenseMovement(
                DeltaSeconds,
                OutAdvancedSeconds,
                OutProcessedBoundary,
                OutEvents,
                OutMovementError);
        },
        [this](const FGuid& EntityId, FAkUGCRuntimeHealth& OutHealth)
        {
            return GetRuntimeHealth(EntityId, OutHealth);
        },
        [this]()
        {
            return HasActiveEnemyMovement();
        },
        [this]()
        {
            ResetTowerDefenseGameplay();
        },
        [WeakLifetime, SessionLifetime]()
        {
            const TSharedPtr<bool, ESPMode::ThreadSafe> RuntimeLifetime = WeakLifetime.Pin();
            const TSharedPtr<bool, ESPMode::ThreadSafe> ActiveSession = SessionLifetime.Pin();
            return RuntimeLifetime.IsValid()
                && *RuntimeLifetime
                && ActiveSession.IsValid()
                && *ActiveSession;
        },
        &HandlerError))
    {
        return Fail(OutError, MoveTemp(HandlerError));
    }

    const bool bHasWaveStart = Scene.LogicGraph.Nodes.ContainsByPredicate([](const FAkUGCLogicNode& Node)
    {
        return Node.Type == EAkUGCLogicNodeType::WaveStart;
    });
    if (bHasWaveStart)
    {
        FAkUGCTowerDefenseRulesetRuntimeConfig RulesetConfig;
        FString RulesetError;
        if (!BuildTowerDefenseRulesetRuntimeConfig(Scene, RulesetConfig, RulesetError)
            || !LogicRuntime->ConfigureTowerDefenseWavesForOwner(
                ExecutionOwnerId,
                RulesetConfig,
                [this]()
                {
                    return GetActiveEnemyMovementCount();
                },
                &RulesetError))
        {
            LogicRuntime->ResetLogicRuntimeForOwner(ExecutionOwnerId);
            return Fail(OutError, MoveTemp(RulesetError));
        }
    }

    LogicExecutionOwnerId = ExecutionOwnerId;
    const FAkUGCLogicRuntimeResult Result = LogicRuntime->RunGameStartForOwner(
        ExecutionOwnerId,
        Scene.LogicGraph);
    if (!Result.bSucceeded)
    {
        LogicRuntime->ResetLogicRuntimeForOwner(ExecutionOwnerId);
        LogicExecutionOwnerId.Invalidate();
        return Fail(OutError, FString::Printf(TEXT("%s: %s"), *Result.ErrorPath, *Result.ErrorMessage));
    }
    return true;
}

bool FAkUGCSceneRuntime::ValidateTowerDefenseGameplay(
    const FAkUGCSceneDocument& Scene,
    const FAkUGCPrefabRegistry& Registry,
    FString* OutError) const
{
    const FAkUGCEntityRecord* Base = nullptr;
    const FAkUGCEntityRecord* Goal = nullptr;
    int32 BaseCount = 0;
    int32 GoalCount = 0;
    for (const FAkUGCEntityRecord& Entity : Scene.Entities)
    {
        if (Entity.PrefabId == TEXT("official.gameplay.base"))
        {
            Base = &Entity;
            ++BaseCount;
        }
        else if (Entity.PrefabId == TEXT("official.gameplay.goal"))
        {
            Goal = &Entity;
            ++GoalCount;
        }
    }
    if (BaseCount != 1 || !Base)
    {
        return Fail(OutError, TEXT("Tower defense gameplay requires exactly one official.gameplay.base entity."));
    }
    if (GoalCount != 1 || !Goal)
    {
        return Fail(OutError, TEXT("Tower defense gameplay requires exactly one official.gameplay.goal entity."));
    }

    const FAkUGCComponentRecord* HealthComponent = Base->Components.FindByPredicate([](const FAkUGCComponentRecord& Component)
    {
        return Component.TypeId == TEXT("core.health");
    });
    const FAkUGCValue* MaximumHealth = HealthComponent ? HealthComponent->Properties.Find(TEXT("maxHealth")) : nullptr;
    if (!MaximumHealth
        || MaximumHealth->Type != EAkUGCValueType::Number
        || !FMath::IsFinite(MaximumHealth->NumberValue)
        || MaximumHealth->NumberValue < 1.0
        || MaximumHealth->NumberValue > 100000.0)
    {
        return Fail(OutError, TEXT("Tower defense Base maxHealth must be a Number from 1 to 100000."));
    }

    int32 GoalComponentCount = 0;
    for (const FAkUGCComponentRecord& Component : Goal->Components)
    {
        if (Component.TypeId == TEXT("tower_defense.goal"))
        {
            ++GoalComponentCount;
        }
    }
    if (GoalComponentCount != 1)
    {
        return Fail(OutError, TEXT("Tower defense Goal must contain exactly one tower_defense.goal component."));
    }

    if (Scene.Ruleset.Waves.Num() != AkUGCTowerDefenseRulesetLimits::RequiredWaveCount)
    {
        return Fail(
            OutError,
            FString::Printf(
                TEXT("Playable tower defense requires exactly %d waves."),
                AkUGCTowerDefenseRulesetLimits::RequiredWaveCount));
    }
    if (!FMath::IsFinite(Scene.Ruleset.WaveIntervalSeconds)
        || Scene.Ruleset.WaveIntervalSeconds < 0.0
        || Scene.Ruleset.WaveIntervalSeconds > AkUGCTowerDefenseRulesetLimits::MaxWaveIntervalSeconds)
    {
        return Fail(OutError, TEXT("Tower defense wave interval must be finite and from 0 to 3600 seconds."));
    }

    int64 TotalEnemyCount = 0;
    TSet<FGuid> WaveIds;
    TSet<FGuid> WaveSpawnPointIds;
    for (int32 WaveIndex = 0; WaveIndex < Scene.Ruleset.Waves.Num(); ++WaveIndex)
    {
        const FAkUGCTowerDefenseWave& Wave = Scene.Ruleset.Waves[WaveIndex];
        const FString WavePrefix = FString::Printf(TEXT("Wave %d"), WaveIndex + 1);
        if (!Wave.WaveId.IsValid() || WaveIds.Contains(Wave.WaveId))
        {
            return Fail(OutError, WavePrefix + TEXT(" requires a unique valid WaveId."));
        }
        WaveIds.Add(Wave.WaveId);
        if (!FMath::IsFinite(Wave.StartDelaySeconds)
            || Wave.StartDelaySeconds < 0.0
            || Wave.StartDelaySeconds > AkUGCTowerDefenseRulesetLimits::MaxStartDelaySeconds)
        {
            return Fail(OutError, WavePrefix + TEXT(" start delay must be finite and from 0 to 3600 seconds."));
        }

        const FAkUGCEntityRecord* SpawnPoint = Scene.Entities.FindByPredicate([&Wave](const FAkUGCEntityRecord& Entity)
        {
            return Entity.EntityId == Wave.SpawnPointEntityId;
        });
        if (!SpawnPoint || SpawnPoint->PrefabId != TEXT("official.gameplay.enemy_spawn"))
        {
            return Fail(OutError, WavePrefix + TEXT(" must reference an official.gameplay.enemy_spawn in the same scene."));
        }
        WaveSpawnPointIds.Add(Wave.SpawnPointEntityId);

        const FAkUGCComponentRecord* SpawnComponent = nullptr;
        int32 SpawnComponentCount = 0;
        for (const FAkUGCComponentRecord& Component : SpawnPoint->Components)
        {
            if (Component.TypeId == TEXT("tower_defense.spawn"))
            {
                SpawnComponent = &Component;
                ++SpawnComponentCount;
            }
        }
        if (SpawnComponentCount != 1 || !SpawnComponent)
        {
            return Fail(OutError, WavePrefix + TEXT(" Spawn Point requires exactly one tower_defense.spawn component."));
        }

        const FAkUGCValue* EnemyPrefab = SpawnComponent->Properties.Find(TEXT("enemyPrefab"));
        const FAkUGCValue* EnemyCount = SpawnComponent->Properties.Find(TEXT("enemyCount"));
        const FAkUGCValue* SpawnInterval = SpawnComponent->Properties.Find(TEXT("spawnInterval"));
        if (!EnemyPrefab || EnemyPrefab->Type != EAkUGCValueType::Name || EnemyPrefab->NameValue.IsNone())
        {
            return Fail(OutError, WavePrefix + TEXT(" enemyPrefab must be a valid Name value."));
        }
        if (!EnemyCount
            || EnemyCount->Type != EAkUGCValueType::Integer
            || EnemyCount->IntegerValue < 1
            || EnemyCount->IntegerValue > 500)
        {
            return Fail(OutError, WavePrefix + TEXT(" enemyCount must be an Integer from 1 to 500."));
        }
        if (!SpawnInterval
            || SpawnInterval->Type != EAkUGCValueType::Number
            || !FMath::IsFinite(SpawnInterval->NumberValue)
            || SpawnInterval->NumberValue < 0.1
            || SpawnInterval->NumberValue > 60.0)
        {
            return Fail(OutError, WavePrefix + TEXT(" spawnInterval must be a Number from 0.1 to 60 seconds."));
        }

        const FAkUGCPrefabDefinition* EnemyDefinition = Registry.Find(EnemyPrefab->NameValue);
        if (!EnemyDefinition || EnemyDefinition->PrefabId != TEXT("official.unit.basic_enemy"))
        {
            return Fail(OutError, WavePrefix + TEXT(" must use the supported official.unit.basic_enemy Prefab."));
        }
        FAkUGCEntityRecord EnemyDefaults;
        FString EnemyDefaultsError;
        if (!Registry.CreateEntityRecord(
            EnemyPrefab->NameValue,
            FGuid::NewGuid(),
            FTransform::Identity,
            EnemyDefaults,
            &EnemyDefaultsError))
        {
            return Fail(OutError, WavePrefix + TEXT(" Enemy Prefab defaults are invalid: ") + EnemyDefaultsError);
        }
        const FAkUGCComponentRecord* EnemyHealth = EnemyDefaults.Components.FindByPredicate([](const FAkUGCComponentRecord& Component)
        {
            return Component.TypeId == TEXT("core.health");
        });
        const FAkUGCComponentRecord* EnemyGameplay = EnemyDefaults.Components.FindByPredicate([](const FAkUGCComponentRecord& Component)
        {
            return Component.TypeId == TEXT("tower_defense.enemy");
        });
        const FAkUGCValue* EnemyMaximumHealth = EnemyHealth ? EnemyHealth->Properties.Find(TEXT("maxHealth")) : nullptr;
        const FAkUGCValue* EnemyMoveSpeed = EnemyGameplay ? EnemyGameplay->Properties.Find(TEXT("moveSpeed")) : nullptr;
        const FAkUGCValue* EnemyGoalDamage = EnemyGameplay ? EnemyGameplay->Properties.Find(TEXT("goalDamage")) : nullptr;
        if (!EnemyMaximumHealth
            || EnemyMaximumHealth->Type != EAkUGCValueType::Number
            || !FMath::IsFinite(EnemyMaximumHealth->NumberValue)
            || EnemyMaximumHealth->NumberValue < 1.0
            || EnemyMaximumHealth->NumberValue > 100000.0
            || !EnemyMoveSpeed
            || EnemyMoveSpeed->Type != EAkUGCValueType::Number
            || !FMath::IsFinite(EnemyMoveSpeed->NumberValue)
            || EnemyMoveSpeed->NumberValue < 10.0
            || EnemyMoveSpeed->NumberValue > 2000.0
            || !EnemyGoalDamage
            || EnemyGoalDamage->Type != EAkUGCValueType::Number
            || !FMath::IsFinite(EnemyGoalDamage->NumberValue)
            || EnemyGoalDamage->NumberValue < 0.0
            || EnemyGoalDamage->NumberValue > 100000.0)
        {
            return Fail(OutError, WavePrefix + TEXT(" Basic Enemy Prefab has invalid health or movement configuration."));
        }

        TotalEnemyCount += EnemyCount->IntegerValue;
        if (TotalEnemyCount > AkUGCTowerDefenseRulesetLimits::MaxTotalEnemyCount)
        {
            return Fail(
                OutError,
                FString::Printf(
                    TEXT("Tower defense Ruleset exceeds the total enemy budget of %d."),
                    AkUGCTowerDefenseRulesetLimits::MaxTotalEnemyCount));
        }
    }

    for (int32 NodeIndex = 0; NodeIndex < Scene.LogicGraph.Nodes.Num(); ++NodeIndex)
    {
        const FAkUGCLogicNode& Node = Scene.LogicGraph.Nodes[NodeIndex];
        if (Node.Type == EAkUGCLogicNodeType::Spawn
            && (!Node.SpawnAtEntityId.IsValid() || !WaveSpawnPointIds.Contains(Node.SpawnAtEntityId)))
        {
            return Fail(
                OutError,
                FString::Printf(
                    TEXT("Tower defense Spawn node %d must reference a Spawn Point used by the Ruleset."),
                    NodeIndex));
        }
    }
    return true;
}

bool FAkUGCSceneRuntime::InitializeTowerDefenseGameplay(
    const FAkUGCSceneDocument& Scene,
    const FAkUGCPrefabRegistry& Registry,
    FString* OutError)
{
    if (!ValidateTowerDefenseGameplay(Scene, Registry, OutError))
    {
        return false;
    }

    const FAkUGCEntityRecord* Base = Scene.Entities.FindByPredicate([](const FAkUGCEntityRecord& Entity)
    {
        return Entity.PrefabId == TEXT("official.gameplay.base");
    });
    const FAkUGCEntityRecord* Goal = Scene.Entities.FindByPredicate([](const FAkUGCEntityRecord& Entity)
    {
        return Entity.PrefabId == TEXT("official.gameplay.goal");
    });
    ResetTowerDefenseGameplay();
    TowerDefenseBaseEntityId = Base->EntityId;
    TowerDefenseGoalEntityId = Goal->EntityId;
    FString GameplayError;
    if (!InitializeRuntimeHealth(*Base, GameplayError))
    {
        ResetTowerDefenseGameplay();
        return Fail(OutError, GameplayError);
    }
    for (const FAkUGCEntityRecord& Entity : Scene.Entities)
    {
        if (Entity.PrefabId == TEXT("official.tower.basic")
            && !RegisterBasicTowerAttack(Entity, GameplayError))
        {
            ResetTowerDefenseGameplay();
            return Fail(OutError, GameplayError);
        }
    }
    return true;
}

void FAkUGCSceneRuntime::ResetTowerDefenseGameplay()
{
    const TArray<FGuid> SpawnedEntityIds = RuntimeSpawnedEntityIds.Array();
    for (const FGuid& EntityId : SpawnedEntityIds)
    {
        RemoveEntity(EntityId);
    }
    RuntimeSpawnedEntityIds.Reset();
    ResetTowerDefenseMovement();
    TowerDefenseBaseEntityId.Invalidate();
    TowerDefenseGoalEntityId.Invalidate();
    RuntimeHealthByEntityId.Reset();
    DeadEntityIds.Reset();
    BasicTowerAttacks.Reset();
}

bool FAkUGCSceneRuntime::BuildLogicSpawnPlan(
    const FAkUGCLogicSpawnEffect& SpawnEffect,
    const FAkUGCPrefabRegistry& Registry,
    FAkUGCLogicSpawnPlan& OutPlan,
    FString& OutError) const
{
    OutPlan = FAkUGCLogicSpawnPlan{};
    OutPlan.SourceNodeId = SpawnEffect.SourceNodeId;
    OutPlan.PrefabId = SpawnEffect.PrefabId;
    OutPlan.SpawnAtEntityId = SpawnEffect.SpawnAtEntityId;
    if (!Registry.Find(OutPlan.PrefabId))
    {
        OutError = FString::Printf(TEXT("Spawn Prefab '%s' is not registered."), *OutPlan.PrefabId.ToString());
        return false;
    }
    if (!SpawnEffect.SpawnAtEntityId.IsValid())
    {
        return true;
    }

    AActor* AnchorActor = FindActor(SpawnEffect.SpawnAtEntityId);
    const UAkUGCEntityBindingComponent* Binding = FindBinding(AnchorActor);
    if (!Binding)
    {
        OutError = TEXT("Logic Spawn anchor does not exist in the runtime scene.");
        return false;
    }
    if (Binding->PrefabId != TEXT("official.gameplay.enemy_spawn"))
    {
        return true;
    }

    const FAkUGCComponentRecord* SpawnComponent = Binding->SourceRecord.Components.FindByPredicate(
        [](const FAkUGCComponentRecord& Component)
        {
            return Component.TypeId == TEXT("tower_defense.spawn");
        });
    if (!SpawnComponent)
    {
        OutError = TEXT("Enemy Spawn anchor is missing tower_defense.spawn configuration.");
        return false;
    }

    const FAkUGCValue* EnemyPrefab = SpawnComponent->Properties.Find(TEXT("enemyPrefab"));
    const FAkUGCValue* EnemyCount = SpawnComponent->Properties.Find(TEXT("enemyCount"));
    const FAkUGCValue* SpawnInterval = SpawnComponent->Properties.Find(TEXT("spawnInterval"));
    if (!EnemyPrefab || EnemyPrefab->Type != EAkUGCValueType::Name || EnemyPrefab->NameValue.IsNone())
    {
        OutError = TEXT("Enemy Spawn enemyPrefab must be a valid Name value.");
        return false;
    }
    if (!EnemyCount
        || EnemyCount->Type != EAkUGCValueType::Integer
        || EnemyCount->IntegerValue < 1
        || EnemyCount->IntegerValue > 500)
    {
        OutError = TEXT("Enemy Spawn enemyCount must be an Integer from 1 to 500.");
        return false;
    }
    if (!SpawnInterval
        || SpawnInterval->Type != EAkUGCValueType::Number
        || !FMath::IsFinite(SpawnInterval->NumberValue)
        || SpawnInterval->NumberValue < 0.1
        || SpawnInterval->NumberValue > 60.0)
    {
        OutError = TEXT("Enemy Spawn spawnInterval must be a Number from 0.1 to 60 seconds.");
        return false;
    }
    if (!Registry.Find(EnemyPrefab->NameValue))
    {
        OutError = FString::Printf(TEXT("Enemy Prefab '%s' is not registered."), *EnemyPrefab->NameValue.ToString());
        return false;
    }

    OutPlan.PrefabId = EnemyPrefab->NameValue;
    OutPlan.Count = static_cast<int32>(EnemyCount->IntegerValue);
    OutPlan.IntervalSeconds = SpawnInterval->NumberValue;
    return true;
}

bool FAkUGCSceneRuntime::SpawnLogicPrefab(
    const FAkUGCLogicSpawnEffect& SpawnEffect,
    const FAkUGCPrefabRegistry& Registry,
    FGuid& OutEntityId,
    FString& OutError)
{
    OutEntityId.Invalidate();
    UWorld* RuntimeWorld = World.Get();
    if (!RuntimeWorld || RuntimeWorld->GetNetMode() == NM_Client)
    {
        OutError = TEXT("Logic Spawn effects require an authoritative runtime world.");
        return false;
    }
    if (SpawnEffect.PrefabId == TEXT("official.unit.basic_enemy") && TowerDefensePath.Num() < 2)
    {
        OutError = TEXT("Basic Enemy Spawn requires at least two path nodes.");
        return false;
    }

    FTransform SpawnTransform = FTransform::Identity;
    if (SpawnEffect.SpawnAtEntityId.IsValid())
    {
        const AActor* AnchorActor = FindActor(SpawnEffect.SpawnAtEntityId);
        if (!AnchorActor)
        {
            OutError = TEXT("Logic Spawn anchor does not exist in the runtime scene.");
            return false;
        }
        SpawnTransform = AnchorActor->GetActorTransform();
    }

    OutEntityId = FGuid::NewGuid();
    FAkUGCEntityRecord Entity;
    if (!Registry.CreateEntityRecord(
        SpawnEffect.PrefabId,
        OutEntityId,
        SpawnTransform,
        Entity,
        &OutError)
        || !SpawnEntity(Entity, Registry, &OutError))
    {
        OutEntityId.Invalidate();
        return false;
    }
    if (!RegisterEnemyMovement(SpawnEffect, Entity, OutError)
        || !InitializeRuntimeHealth(Entity, OutError))
    {
        RemoveEntity(OutEntityId);
        OutEntityId.Invalidate();
        return false;
    }
    RuntimeSpawnedEntityIds.Add(OutEntityId);
    return true;
}

bool FAkUGCSceneRuntime::SpawnSandboxEntity(
    const FName& PrefabId,
    const FGuid& AnchorEntityId,
    const FAkUGCPrefabRegistry& Registry,
    FGuid& OutEntityId,
    FString& OutError)
{
    FAkUGCLogicSpawnEffect SpawnEffect;
    SpawnEffect.PrefabId = PrefabId;
    SpawnEffect.SpawnAtEntityId = AnchorEntityId;
    return SpawnLogicPrefab(SpawnEffect, Registry, OutEntityId, OutError);
}

bool FAkUGCSceneRuntime::GetWaveRuntimeState(FAkUGCWaveRuntimeSnapshot& OutSnapshot) const
{
    const UWorld* RuntimeWorld = World.Get();
    if (!RuntimeWorld)
    {
        return false;
    }
    const UAkUGCLogicRuntimeSubsystem* LogicRuntime = RuntimeWorld->GetSubsystem<UAkUGCLogicRuntimeSubsystem>();
    if (!LogicRuntime)
    {
        return false;
    }
    OutSnapshot = LogicRuntime->GetWaveRuntimeState();
    return true;
}

bool FAkUGCSceneRuntime::InitializeRuntimeHealth(
    const FAkUGCEntityRecord& Entity,
    FString& OutError)
{
    const FAkUGCComponentRecord* HealthComponent = nullptr;
    int32 HealthComponentCount = 0;
    for (const FAkUGCComponentRecord& Component : Entity.Components)
    {
        if (Component.TypeId == TEXT("core.health"))
        {
            HealthComponent = &Component;
            ++HealthComponentCount;
        }
    }
    if (HealthComponentCount == 0)
    {
        return true;
    }
    if (HealthComponentCount != 1)
    {
        OutError = TEXT("Runtime health requires exactly one core.health component.");
        return false;
    }

    const FAkUGCValue* MaximumHealth = HealthComponent->Properties.Find(TEXT("maxHealth"));
    if (!MaximumHealth
        || MaximumHealth->Type != EAkUGCValueType::Number
        || !FMath::IsFinite(MaximumHealth->NumberValue)
        || MaximumHealth->NumberValue < 1.0
        || MaximumHealth->NumberValue > 100000.0)
    {
        OutError = TEXT("Runtime maxHealth must be a Number from 1 to 100000.");
        return false;
    }

    FAkUGCRuntimeHealth Health;
    Health.Maximum = MaximumHealth->NumberValue;
    Health.Current = MaximumHealth->NumberValue;
    RuntimeHealthByEntityId.Add(Entity.EntityId, Health);
    DeadEntityIds.Remove(Entity.EntityId);
    return true;
}

bool FAkUGCSceneRuntime::BuildTowerDefenseRulesetRuntimeConfig(
    const FAkUGCSceneDocument& Scene,
    FAkUGCTowerDefenseRulesetRuntimeConfig& OutConfig,
    FString& OutError) const
{
    OutConfig = FAkUGCTowerDefenseRulesetRuntimeConfig{};
    OutConfig.BaseEntityId = TowerDefenseBaseEntityId;
    OutConfig.WaveIntervalSeconds = Scene.Ruleset.WaveIntervalSeconds;
    OutConfig.DefeatCondition = Scene.Ruleset.DefeatCondition;
    OutConfig.VictoryCondition = Scene.Ruleset.VictoryCondition;
    for (const FAkUGCTowerDefenseWave& Wave : Scene.Ruleset.Waves)
    {
        const FAkUGCEntityRecord* SpawnPoint = Scene.Entities.FindByPredicate([&Wave](const FAkUGCEntityRecord& Entity)
        {
            return Entity.EntityId == Wave.SpawnPointEntityId;
        });
        const FAkUGCComponentRecord* SpawnComponent = SpawnPoint
            ? SpawnPoint->Components.FindByPredicate([](const FAkUGCComponentRecord& Component)
            {
                return Component.TypeId == TEXT("tower_defense.spawn");
            })
            : nullptr;
        const FAkUGCValue* EnemyPrefab = SpawnComponent ? SpawnComponent->Properties.Find(TEXT("enemyPrefab")) : nullptr;
        const FAkUGCValue* EnemyCount = SpawnComponent ? SpawnComponent->Properties.Find(TEXT("enemyCount")) : nullptr;
        const FAkUGCValue* SpawnInterval = SpawnComponent ? SpawnComponent->Properties.Find(TEXT("spawnInterval")) : nullptr;
        if (!EnemyPrefab || !EnemyCount || !SpawnInterval)
        {
            OutError = TEXT("Tower defense Ruleset contains an invalid Spawn Point configuration.");
            OutConfig = FAkUGCTowerDefenseRulesetRuntimeConfig{};
            return false;
        }

        FAkUGCTowerDefenseWaveRuntimeConfig& RuntimeWave = OutConfig.Waves.AddDefaulted_GetRef();
        RuntimeWave.WaveId = Wave.WaveId;
        RuntimeWave.SpawnPointEntityId = Wave.SpawnPointEntityId;
        RuntimeWave.EnemyPrefabId = EnemyPrefab->NameValue;
        RuntimeWave.EnemyCount = static_cast<int32>(EnemyCount->IntegerValue);
        RuntimeWave.SpawnIntervalSeconds = SpawnInterval->NumberValue;
        RuntimeWave.StartDelaySeconds = Wave.StartDelaySeconds;
    }
    return true;
}

bool FAkUGCSceneRuntime::RegisterBasicTowerAttack(
    const FAkUGCEntityRecord& Entity,
    FString& OutError)
{
    const FAkUGCComponentRecord* TowerComponent = nullptr;
    int32 TowerComponentCount = 0;
    for (const FAkUGCComponentRecord& Component : Entity.Components)
    {
        if (Component.TypeId == TEXT("tower_defense.tower"))
        {
            TowerComponent = &Component;
            ++TowerComponentCount;
        }
    }
    if (TowerComponentCount != 1)
    {
        OutError = TEXT("Basic Tower requires exactly one tower_defense.tower component.");
        return false;
    }

    const FAkUGCValue* AttackRange = TowerComponent->Properties.Find(TEXT("attackRange"));
    const FAkUGCValue* AttackInterval = TowerComponent->Properties.Find(TEXT("attackInterval"));
    const FAkUGCValue* AttackDamage = TowerComponent->Properties.Find(TEXT("attackDamage"));
    if (!AttackRange
        || AttackRange->Type != EAkUGCValueType::Number
        || !FMath::IsFinite(AttackRange->NumberValue)
        || AttackRange->NumberValue < 10.0
        || AttackRange->NumberValue > 10000.0)
    {
        OutError = TEXT("Basic Tower attackRange must be a Number from 10 to 10000.");
        return false;
    }
    if (!AttackInterval
        || AttackInterval->Type != EAkUGCValueType::Number
        || !FMath::IsFinite(AttackInterval->NumberValue)
        || AttackInterval->NumberValue < 0.05
        || AttackInterval->NumberValue > 60.0)
    {
        OutError = TEXT("Basic Tower attackInterval must be a Number from 0.05 to 60 seconds.");
        return false;
    }
    if (!AttackDamage
        || AttackDamage->Type != EAkUGCValueType::Number
        || !FMath::IsFinite(AttackDamage->NumberValue)
        || AttackDamage->NumberValue < 0.0
        || AttackDamage->NumberValue > 100000.0)
    {
        OutError = TEXT("Basic Tower attackDamage must be a Number from 0 to 100000.");
        return false;
    }
    if (!FindActor(Entity.EntityId))
    {
        OutError = TEXT("Basic Tower does not have a runtime Actor.");
        return false;
    }

    FAkUGCTowerDefenseBasicTowerAttack Attack;
    Attack.EntityId = Entity.EntityId;
    Attack.AttackRange = AttackRange->NumberValue;
    Attack.AttackInterval = AttackInterval->NumberValue;
    Attack.AttackDamage = AttackDamage->NumberValue;
    Attack.RemainingAttackSeconds = Attack.AttackInterval;
    BasicTowerAttacks.Add(Entity.EntityId, MoveTemp(Attack));
    return true;
}

bool FAkUGCSceneRuntime::SelectBasicTowerTarget(
    const FAkUGCTowerDefenseBasicTowerAttack& Tower,
    FGuid& OutTargetEntityId) const
{
    OutTargetEntityId.Invalidate();
    const AActor* TowerActor = FindActor(Tower.EntityId);
    if (!TowerActor)
    {
        return false;
    }

    double BestRemainingDistance = TNumericLimits<double>::Max();
    TArray<FGuid> EnemyEntityIds;
    EnemyMovements.GetKeys(EnemyEntityIds);
    EnemyEntityIds.Sort([](const FGuid& Left, const FGuid& Right)
    {
        return Left.ToString(EGuidFormats::Digits) < Right.ToString(EGuidFormats::Digits);
    });
    for (const FGuid& EnemyEntityId : EnemyEntityIds)
    {
        const AActor* EnemyActor = FindActor(EnemyEntityId);
        const FAkUGCRuntimeHealth* Health = RuntimeHealthByEntityId.Find(EnemyEntityId);
        double RemainingDistance = 0.0;
        if (!EnemyActor
            || !Health
            || Health->Current <= 0.0
            || FVector::DistSquared(TowerActor->GetActorLocation(), EnemyActor->GetActorLocation())
                > FMath::Square(Tower.AttackRange)
            || !GetEnemyRemainingPathDistance(EnemyEntityId, RemainingDistance))
        {
            continue;
        }
        if (!OutTargetEntityId.IsValid() || RemainingDistance < BestRemainingDistance)
        {
            OutTargetEntityId = EnemyEntityId;
            BestRemainingDistance = RemainingDistance;
        }
    }
    return OutTargetEntityId.IsValid();
}

bool FAkUGCSceneRuntime::RegisterEnemyMovement(
    const FAkUGCLogicSpawnEffect& SpawnEffect,
    const FAkUGCEntityRecord& Entity,
    FString& OutError)
{
    if (Entity.PrefabId != TEXT("official.unit.basic_enemy"))
    {
        return true;
    }
    if (TowerDefensePath.Num() < 2)
    {
        OutError = TEXT("Basic Enemy movement requires at least two path nodes.");
        return false;
    }

    const FAkUGCComponentRecord* EnemyComponent = Entity.Components.FindByPredicate([](const FAkUGCComponentRecord& Component)
    {
        return Component.TypeId == TEXT("tower_defense.enemy");
    });
    const FAkUGCValue* MoveSpeed = EnemyComponent ? EnemyComponent->Properties.Find(TEXT("moveSpeed")) : nullptr;
    const FAkUGCValue* GoalDamage = EnemyComponent ? EnemyComponent->Properties.Find(TEXT("goalDamage")) : nullptr;
    if (!MoveSpeed
        || MoveSpeed->Type != EAkUGCValueType::Number
        || !FMath::IsFinite(MoveSpeed->NumberValue)
        || MoveSpeed->NumberValue < 10.0
        || MoveSpeed->NumberValue > 2000.0)
    {
        OutError = TEXT("Basic Enemy moveSpeed must be a Number from 10 to 2000.");
        return false;
    }
    if (!GoalDamage
        || GoalDamage->Type != EAkUGCValueType::Number
        || !FMath::IsFinite(GoalDamage->NumberValue)
        || GoalDamage->NumberValue < 0.0
        || GoalDamage->NumberValue > 100000.0)
    {
        OutError = TEXT("Basic Enemy goalDamage must be a Number from 0 to 100000.");
        return false;
    }

    FAkUGCTowerDefenseEnemyMovement Movement;
    Movement.SourceNodeId = SpawnEffect.SourceNodeId;
    Movement.EntityId = Entity.EntityId;
    Movement.MoveSpeed = MoveSpeed->NumberValue;
    Movement.GoalDamage = GoalDamage->NumberValue;
    EnemyMovements.Add(Entity.EntityId, MoveTemp(Movement));
    return true;
}

bool FAkUGCSceneRuntime::SynchronizeScene(
    const FAkUGCSceneDocument& Scene,
    const FAkUGCPrefabRegistry& Registry,
    FString* OutError)
{
    if (!ValidateScene(Scene, Registry, OutError))
    {
        return false;
    }
    const FAkUGCTowerDefensePathBuildResult PathResult = FAkUGCTowerDefensePathBuilder::Build(Scene);
    if (!PathResult.bSucceeded)
    {
        return Fail(OutError, FString::Printf(TEXT("%s: %s"), *PathResult.ErrorPath, *PathResult.ErrorMessage));
    }
    if (ActiveSceneId != Scene.SceneId)
    {
        return LoadScene(Scene, Registry, OutError);
    }

    TSet<FGuid> DesiredEntityIds;
    for (const FAkUGCEntityRecord& Entity : Scene.Entities)
    {
        DesiredEntityIds.Add(Entity.EntityId);
    }

    TArray<FGuid> ExistingEntityIds;
    Actors.GetKeys(ExistingEntityIds);
    for (const FGuid& ExistingEntityId : ExistingEntityIds)
    {
        if (!DesiredEntityIds.Contains(ExistingEntityId))
        {
            RemoveEntity(ExistingEntityId);
        }
    }

    for (const TPair<FGuid, TWeakObjectPtr<AActor>>& Pair : Actors)
    {
        if (AActor* Actor = Pair.Value.Get())
        {
            Actor->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
        }
    }

    for (const FAkUGCEntityRecord& Entity : Scene.Entities)
    {
        AActor* ExistingActor = FindActor(Entity.EntityId);
        const UAkUGCEntityBindingComponent* Binding = FindBinding(ExistingActor);
        if (Binding && Binding->PrefabId != Entity.PrefabId)
        {
            RemoveEntity(Entity.EntityId);
            ExistingActor = nullptr;
        }

        if (ExistingActor)
        {
            if (!ApplyEntity(Entity, Registry, OutError))
            {
                return false;
            }
        }
        else if (!SpawnEntity(Entity, Registry, OutError))
        {
            return false;
        }
    }

    if (!AttachParents(Scene, OutError))
    {
        return false;
    }
    TowerDefensePath = PathResult.Path;
    return true;
}

bool FAkUGCSceneRuntime::ApplyTransaction(
    const FAkUGCCommandTransaction& Transaction,
    const FAkUGCSceneDocument& ResultScene,
    const FAkUGCPrefabRegistry& Registry,
    FString* OutError)
{
    if (!ActiveSceneId.IsValid())
    {
        return Fail(OutError, TEXT("Runtime scene is not initialized."));
    }
    if (ResultScene.SceneId != ActiveSceneId)
    {
        return Fail(OutError, TEXT("Result scene does not match the active runtime scene."));
    }
    const FAkUGCTowerDefensePathBuildResult PathResult = FAkUGCTowerDefensePathBuilder::Build(ResultScene);
    if (!PathResult.bSucceeded)
    {
        return Fail(OutError, FString::Printf(TEXT("%s: %s"), *PathResult.ErrorPath, *PathResult.ErrorMessage));
    }

    TSet<FGuid> AttachmentUpdates;
    for (const FAkUGCCommand& Command : Transaction.Commands)
    {
        if (Command.SceneId != ActiveSceneId)
        {
            return Fail(OutError, TEXT("Command targets a different scene than the active runtime scene."));
        }
        if (Command.Type == EAkUGCCommandType::SetTransform
            || Command.Type == EAkUGCCommandType::SetParent)
        {
            if (AActor* Actor = FindActor(Command.EntityId))
            {
                if (Command.Type == EAkUGCCommandType::SetTransform)
                {
                    TArray<AActor*> Descendants;
                    Actor->GetAttachedActors(Descendants, false, true);
                    for (AActor* Descendant : Descendants)
                    {
                        if (const UAkUGCEntityBindingComponent* Binding = FindBinding(Descendant))
                        {
                            Descendant->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
                            AttachmentUpdates.Add(Binding->EntityId);
                        }
                    }
                }
                Actor->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
                AttachmentUpdates.Add(Command.EntityId);
            }
        }
    }

    for (const FAkUGCCommand& Command : Transaction.Commands)
    {
        if (!ApplyCommand(Command, Registry, AttachmentUpdates, OutError))
        {
            return false;
        }
    }

    if (!RefreshAttachments(AttachmentUpdates, OutError))
    {
        return false;
    }
    TowerDefensePath = PathResult.Path;
    return true;
}

bool FAkUGCSceneRuntime::ApplyEntity(
    const FAkUGCEntityRecord& Entity,
    const FAkUGCPrefabRegistry& Registry,
    FString* OutError)
{
    AActor* Actor = FindActor(Entity.EntityId);
    if (!Actor)
    {
        return SpawnEntity(Entity, Registry, OutError);
    }

    UAkUGCEntityBindingComponent* Binding = FindBinding(Actor);
    if (!Binding)
    {
        return Fail(OutError, TEXT("Runtime actor does not contain a UGC binding component."));
    }
    if (Binding->PrefabId != Entity.PrefabId)
    {
        return Fail(OutError, TEXT("Changing PrefabId in place is not supported; remove and add the entity instead."));
    }

    Binding->ApplyRecord(Entity);
    return true;
}

bool FAkUGCSceneRuntime::RemoveEntity(const FGuid& EntityId)
{
    TWeakObjectPtr<AActor>* ActorPtr = Actors.Find(EntityId);
    if (!ActorPtr)
    {
        return false;
    }

    if (AActor* Actor = ActorPtr->Get())
    {
        TArray<AActor*> AttachedActors;
        Actor->GetAttachedActors(AttachedActors);
        for (AActor* AttachedActor : AttachedActors)
        {
            if (AttachedActor)
            {
                AttachedActor->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
            }
        }
        Actor->Destroy();
    }
    Actors.Remove(EntityId);
    ExternallyDeletedEntityIds.Remove(EntityId);
    BasicTowerAttacks.Remove(EntityId);
    EnemyMovements.Remove(EntityId);
    RuntimeSpawnedEntityIds.Remove(EntityId);
    RuntimeHealthByEntityId.Remove(EntityId);
    DeadEntityIds.Remove(EntityId);
    return true;
}

bool FAkUGCSceneRuntime::NotifyActorDeletedExternally(const FGuid& EntityId, const AActor* Actor)
{
    const TWeakObjectPtr<AActor>* ExistingActor = Actors.Find(EntityId);
    if (!ExistingActor || ExistingActor->Get() != Actor)
    {
        return false;
    }

    Actors.Remove(EntityId);
    ExternallyDeletedEntityIds.Add(EntityId);
    BasicTowerAttacks.Remove(EntityId);
    EnemyMovements.Remove(EntityId);
    RuntimeSpawnedEntityIds.Remove(EntityId);
    RuntimeHealthByEntityId.Remove(EntityId);
    DeadEntityIds.Remove(EntityId);
    return true;
}

void FAkUGCSceneRuntime::CancelLogicExecution(const FGuid& ExecutionOwnerId)
{
    if (!ExecutionOwnerId.IsValid() || LogicExecutionOwnerId != ExecutionOwnerId)
    {
        return;
    }
    if (UWorld* RuntimeWorld = World.Get())
    {
        if (UAkUGCLogicRuntimeSubsystem* LogicRuntime = RuntimeWorld->GetSubsystem<UAkUGCLogicRuntimeSubsystem>())
        {
            LogicRuntime->ResetLogicRuntimeForOwner(ExecutionOwnerId);
        }
    }
    ResetTowerDefenseGameplay();
    LogicExecutionOwnerId.Invalidate();
}

void FAkUGCSceneRuntime::Unload()
{
    CancelLogicExecution(LogicExecutionOwnerId);

    for (TPair<FGuid, TWeakObjectPtr<AActor>>& Pair : Actors)
    {
        if (AActor* Actor = Pair.Value.Get())
        {
            Actor->Destroy();
        }
    }
    Actors.Reset();
    ExternallyDeletedEntityIds.Reset();
    ResetTowerDefenseGameplay();
    TowerDefensePath = FAkUGCTowerDefensePath{};
    ActiveSceneId.Invalidate();
}

AActor* FAkUGCSceneRuntime::FindActor(const FGuid& EntityId) const
{
    const TWeakObjectPtr<AActor>* Actor = Actors.Find(EntityId);
    return Actor ? Actor->Get() : nullptr;
}

int32 FAkUGCSceneRuntime::Num() const
{
    return Actors.Num();
}

FGuid FAkUGCSceneRuntime::GetActiveSceneId() const
{
    return ActiveSceneId;
}

const FAkUGCTowerDefensePath& FAkUGCSceneRuntime::GetTowerDefensePath() const
{
    return TowerDefensePath;
}

int32 FAkUGCSceneRuntime::GetActiveEnemyMovementCount() const
{
    return EnemyMovements.Num();
}

double FAkUGCSceneRuntime::GetBaseCurrentHealth() const
{
    const FAkUGCRuntimeHealth* Health = RuntimeHealthByEntityId.Find(TowerDefenseBaseEntityId);
    return Health ? Health->Current : 0.0;
}

double FAkUGCSceneRuntime::GetBaseMaximumHealth() const
{
    const FAkUGCRuntimeHealth* Health = RuntimeHealthByEntityId.Find(TowerDefenseBaseEntityId);
    return Health ? Health->Maximum : 0.0;
}

bool FAkUGCSceneRuntime::GetRuntimeHealth(
    const FGuid& EntityId,
    FAkUGCRuntimeHealth& OutHealth) const
{
    const FAkUGCRuntimeHealth* Health = RuntimeHealthByEntityId.Find(EntityId);
    if (!Health)
    {
        OutHealth = FAkUGCRuntimeHealth{};
        return false;
    }
    OutHealth = *Health;
    return true;
}

bool FAkUGCSceneRuntime::ApplyRuntimeDamage(
    const FGuid& SourceEntityId,
    const FGuid& TargetEntityId,
    double Damage,
    FAkUGCRuntimeDamage& OutDamage,
    FString& OutError)
{
    OutDamage = FAkUGCRuntimeDamage{};
    const UWorld* RuntimeWorld = World.Get();
    if (!RuntimeWorld || RuntimeWorld->GetNetMode() == NM_Client)
    {
        OutError = TEXT("Runtime damage requires an authoritative runtime world.");
        return false;
    }
    if (!TargetEntityId.IsValid())
    {
        OutError = TEXT("Runtime damage requires a valid target EntityId.");
        return false;
    }
    if (!FMath::IsFinite(Damage) || Damage < 0.0 || Damage > 100000.0)
    {
        OutError = TEXT("Runtime damage must be finite and from 0 to 100000.");
        return false;
    }

    FAkUGCRuntimeHealth* Health = RuntimeHealthByEntityId.Find(TargetEntityId);
    if (!Health)
    {
        OutError = TEXT("Runtime damage target does not have active health state.");
        return false;
    }

    OutDamage.SourceEntityId = SourceEntityId;
    OutDamage.TargetEntityId = TargetEntityId;
    OutDamage.RequestedDamage = Damage;
    OutDamage.HealthAfterDamage = Health->Current;
    if (DeadEntityIds.Contains(TargetEntityId) || Health->Current <= 0.0)
    {
        return true;
    }

    const double PreviousHealth = Health->Current;
    Health->Current = FMath::Max(0.0, PreviousHealth - Damage);
    OutDamage.AppliedDamage = PreviousHealth - Health->Current;
    OutDamage.HealthAfterDamage = Health->Current;
    OutDamage.bKilled = Health->Current <= 0.0;
    if (OutDamage.bKilled)
    {
        DeadEntityIds.Add(TargetEntityId);
        if (RuntimeSpawnedEntityIds.Contains(TargetEntityId))
        {
            RemoveEntity(TargetEntityId);
        }
    }
    return true;
}

bool FAkUGCSceneRuntime::GetEnemyRemainingPathDistance(
    const FGuid& EntityId,
    double& OutDistance) const
{
    OutDistance = 0.0;
    const FAkUGCTowerDefenseEnemyMovement* Movement = EnemyMovements.Find(EntityId);
    const AActor* Actor = FindActor(EntityId);
    if (!Movement || !Actor || Movement->NextPathNodeIndex >= TowerDefensePath.Num())
    {
        return false;
    }

    OutDistance = FVector::Distance(
        Actor->GetActorLocation(),
        TowerDefensePath.Nodes[Movement->NextPathNodeIndex].Location);
    for (int32 PathIndex = Movement->NextPathNodeIndex; PathIndex + 1 < TowerDefensePath.Num(); ++PathIndex)
    {
        OutDistance += FVector::Distance(
            TowerDefensePath.Nodes[PathIndex].Location,
            TowerDefensePath.Nodes[PathIndex + 1].Location);
    }
    return FMath::IsFinite(OutDistance);
}

FGuid FAkUGCSceneRuntime::GetTowerDefenseBaseEntityId() const
{
    return TowerDefenseBaseEntityId;
}

FGuid FAkUGCSceneRuntime::GetTowerDefenseGoalEntityId() const
{
    return TowerDefenseGoalEntityId;
}

const TArray<FAkUGCTowerDefenseGoalReached>& FAkUGCSceneRuntime::GetGoalReachedEvents() const
{
    return GoalReachedEvents;
}

bool FAkUGCSceneRuntime::AdvanceTowerDefenseMovement(
    double DeltaSeconds,
    double& OutAdvancedSeconds,
    bool& OutProcessedBoundary,
    FAkUGCTowerDefenseGameplayEvents& OutEvents,
    FString& OutError)
{
    OutAdvancedSeconds = 0.0;
    OutProcessedBoundary = false;
    OutEvents = FAkUGCTowerDefenseGameplayEvents{};
    const UWorld* RuntimeWorld = World.Get();
    if (!RuntimeWorld || RuntimeWorld->GetNetMode() == NM_Client)
    {
        OutError = TEXT("Tower defense gameplay requires an authoritative runtime world.");
        return false;
    }
    if (!FMath::IsFinite(DeltaSeconds) || DeltaSeconds < 0.0)
    {
        OutError = TEXT("Tower defense gameplay delta must be finite and non-negative.");
        return false;
    }
    if (EnemyMovements.IsEmpty())
    {
        return true;
    }
    if (TowerDefensePath.Num() < 2)
    {
        OutError = TEXT("Active tower defense gameplay requires at least two path nodes.");
        return false;
    }
    const FAkUGCRuntimeHealth* BaseHealth = RuntimeHealthByEntityId.Find(TowerDefenseBaseEntityId);
    if (!TowerDefenseBaseEntityId.IsValid()
        || !TowerDefenseGoalEntityId.IsValid()
        || !BaseHealth
        || BaseHealth->Maximum <= 0.0
        || !FindActor(TowerDefenseBaseEntityId)
        || !FindActor(TowerDefenseGoalEntityId))
    {
        OutError = TEXT("Active tower defense gameplay requires initialized Base and Goal runtime state.");
        return false;
    }

    TArray<FGuid> TowerEntityIds;
    BasicTowerAttacks.GetKeys(TowerEntityIds);
    TowerEntityIds.Sort([](const FGuid& Left, const FGuid& Right)
    {
        return Left.ToString(EGuidFormats::Digits) < Right.ToString(EGuidFormats::Digits);
    });
    for (const FGuid& TowerEntityId : TowerEntityIds)
    {
        if (!FindActor(TowerEntityId))
        {
            OutError = FString::Printf(TEXT("Basic Tower '%s' is missing from the runtime scene."), *TowerEntityId.ToString());
            return false;
        }
    }

    TArray<FGuid> EnemyEntityIds;
    EnemyMovements.GetKeys(EnemyEntityIds);
    EnemyEntityIds.Sort([](const FGuid& Left, const FGuid& Right)
    {
        return Left.ToString(EGuidFormats::Digits) < Right.ToString(EGuidFormats::Digits);
    });

    double NextGoalSeconds = TNumericLimits<double>::Max();
    for (const FGuid& EnemyEntityId : EnemyEntityIds)
    {
        const FAkUGCTowerDefenseEnemyMovement* Movement = EnemyMovements.Find(EnemyEntityId);
        double RemainingPathDistance = 0.0;
        if (!Movement
            || !FindActor(EnemyEntityId)
            || !RuntimeHealthByEntityId.Contains(EnemyEntityId)
            || !GetEnemyRemainingPathDistance(EnemyEntityId, RemainingPathDistance))
        {
            OutError = FString::Printf(TEXT("Active enemy '%s' has invalid runtime state."), *EnemyEntityId.ToString());
            return false;
        }
        NextGoalSeconds = FMath::Min(NextGoalSeconds, RemainingPathDistance / Movement->MoveSpeed);
    }

    double NextAttackSeconds = TNumericLimits<double>::Max();
    for (const FGuid& TowerEntityId : TowerEntityIds)
    {
        NextAttackSeconds = FMath::Min(
            NextAttackSeconds,
            BasicTowerAttacks.FindChecked(TowerEntityId).RemainingAttackSeconds);
    }
    const double TimeSlice = FMath::Min3(DeltaSeconds, NextGoalSeconds, NextAttackSeconds);
    if (!FMath::IsFinite(TimeSlice) || TimeSlice < 0.0)
    {
        OutError = TEXT("Tower defense gameplay produced an invalid time slice.");
        return false;
    }
    OutAdvancedSeconds = TimeSlice;

    for (const FGuid& EnemyEntityId : EnemyEntityIds)
    {
        FAkUGCTowerDefenseEnemyMovement& Movement = EnemyMovements.FindChecked(EnemyEntityId);
        AActor* EnemyActor = FindActor(EnemyEntityId);
        double RemainingMovementDistance = Movement.MoveSpeed * TimeSlice;
        while (Movement.NextPathNodeIndex < TowerDefensePath.Num())
        {
            const FVector Target = TowerDefensePath.Nodes[Movement.NextPathNodeIndex].Location;
            const FVector Current = EnemyActor->GetActorLocation();
            const double DistanceToTarget = FVector::Distance(Current, Target);
            if (DistanceToTarget <= UE_KINDA_SMALL_NUMBER)
            {
                EnemyActor->SetActorLocation(Target, false, nullptr, ETeleportType::TeleportPhysics);
                ++Movement.NextPathNodeIndex;
                continue;
            }
            if (RemainingMovementDistance < DistanceToTarget)
            {
                EnemyActor->SetActorLocation(
                    Current + (Target - Current).GetSafeNormal() * RemainingMovementDistance,
                    false,
                    nullptr,
                    ETeleportType::TeleportPhysics);
                break;
            }
            EnemyActor->SetActorLocation(Target, false, nullptr, ETeleportType::TeleportPhysics);
            RemainingMovementDistance -= DistanceToTarget;
            ++Movement.NextPathNodeIndex;
        }
    }
    for (const FGuid& TowerEntityId : TowerEntityIds)
    {
        FAkUGCTowerDefenseBasicTowerAttack& Tower = BasicTowerAttacks.FindChecked(TowerEntityId);
        Tower.RemainingAttackSeconds = FMath::Max(0.0, Tower.RemainingAttackSeconds - TimeSlice);
    }

    for (const FGuid& TowerEntityId : TowerEntityIds)
    {
        FAkUGCTowerDefenseBasicTowerAttack& Tower = BasicTowerAttacks.FindChecked(TowerEntityId);
        if (Tower.RemainingAttackSeconds > 0.0 || EnemyMovements.IsEmpty())
        {
            continue;
        }
        Tower.RemainingAttackSeconds = Tower.AttackInterval;

        FGuid TargetEntityId;
        if (Tower.AttackDamage > 0.0 && SelectBasicTowerTarget(Tower, TargetEntityId))
        {
            FAkUGCRuntimeDamage Damage;
            if (!ApplyRuntimeDamage(
                Tower.EntityId,
                TargetEntityId,
                Tower.AttackDamage,
                Damage,
                OutError))
            {
                return false;
            }
            OutEvents.DamageEvents.Add(Damage);
        }
        OutProcessedBoundary = true;
        return true;
    }

    for (const FGuid& EnemyEntityId : EnemyEntityIds)
    {
        const FAkUGCTowerDefenseEnemyMovement* Movement = EnemyMovements.Find(EnemyEntityId);
        if (!Movement || Movement->NextPathNodeIndex < TowerDefensePath.Num())
        {
            continue;
        }
        FAkUGCRuntimeDamage Damage;
        if (!ApplyRuntimeDamage(
            EnemyEntityId,
            TowerDefenseBaseEntityId,
            Movement->GoalDamage,
            Damage,
            OutError))
        {
            return false;
        }
        OutEvents.DamageEvents.Add(Damage);

        FAkUGCTowerDefenseGoalReached Reached;
        Reached.SourceNodeId = Movement->SourceNodeId;
        Reached.EntityId = EnemyEntityId;
        Reached.GoalEntityId = TowerDefenseGoalEntityId;
        Reached.BaseEntityId = TowerDefenseBaseEntityId;
        Reached.DamageApplied = Damage.AppliedDamage;
        Reached.BaseHealthAfterDamage = Damage.HealthAfterDamage;
        GoalReachedEvents.Add(Reached);
        OutEvents.GoalReachedEvents.Add(Reached);
        RemoveEntity(EnemyEntityId);
        OutProcessedBoundary = true;
        return true;
    }
    return true;
}

bool FAkUGCSceneRuntime::HasActiveEnemyMovement() const
{
    return !EnemyMovements.IsEmpty();
}

void FAkUGCSceneRuntime::ResetTowerDefenseMovement()
{
    EnemyMovements.Reset();
    GoalReachedEvents.Reset();
}

FName FAkUGCSceneRuntime::GetCurrentPlatformVariant()
{
#if UE_SERVER
    return TEXT("Server");
#elif PLATFORM_ANDROID
    return TEXT("Android");
#elif PLATFORM_IOS
    return TEXT("IOS");
#elif PLATFORM_WINDOWS
    return TEXT("Win64");
#else
    return TEXT("Default");
#endif
}

bool FAkUGCSceneRuntime::ValidateScene(
    const FAkUGCSceneDocument& Scene,
    const FAkUGCPrefabRegistry& Registry,
    FString* OutError) const
{
    if (!World.IsValid())
    {
        return Fail(OutError, TEXT("Runtime world is not valid."));
    }
    if (!Scene.SceneId.IsValid())
    {
        return Fail(OutError, TEXT("Scene ID must be a valid GUID."));
    }

    TSet<FGuid> EntityIds;
    TMap<FGuid, FGuid> ParentByEntity;
    for (const FAkUGCEntityRecord& Entity : Scene.Entities)
    {
        if (!Entity.EntityId.IsValid())
        {
            return Fail(OutError, TEXT("Scene contains an invalid entity ID."));
        }
        if (EntityIds.Contains(Entity.EntityId))
        {
            return Fail(OutError, TEXT("Scene contains a duplicate entity ID."));
        }
        if (!Registry.Find(Entity.PrefabId))
        {
            return Fail(OutError, FString::Printf(TEXT("Prefab '%s' is not registered."), *Entity.PrefabId.ToString()));
        }
        EntityIds.Add(Entity.EntityId);
        ParentByEntity.Add(Entity.EntityId, Entity.ParentEntityId);
    }

    for (const FAkUGCEntityRecord& Entity : Scene.Entities)
    {
        if (Entity.ParentEntityId.IsValid() && !EntityIds.Contains(Entity.ParentEntityId))
        {
            return Fail(OutError, FString::Printf(
                TEXT("Parent '%s' for entity '%s' is not part of the scene."),
                *Entity.ParentEntityId.ToString(),
                *Entity.EntityId.ToString()));
        }
        if (Entity.ParentEntityId == Entity.EntityId)
        {
            return Fail(OutError, TEXT("Entity cannot be parented to itself."));
        }
    }

    TMap<FGuid, uint8> VisitStates;
    TFunction<bool(const FGuid&)> VisitParent = [&](const FGuid& EntityId)
    {
        const uint8 State = VisitStates.FindRef(EntityId);
        if (State == 1)
        {
            return false;
        }
        if (State == 2)
        {
            return true;
        }

        VisitStates.Add(EntityId, 1);
        const FGuid ParentId = ParentByEntity.FindRef(EntityId);
        if (ParentId.IsValid() && !VisitParent(ParentId))
        {
            return false;
        }
        VisitStates.Add(EntityId, 2);
        return true;
    };

    for (const TPair<FGuid, FGuid>& Pair : ParentByEntity)
    {
        if (!VisitParent(Pair.Key))
        {
            return Fail(OutError, TEXT("Parent hierarchy contains a cycle."));
        }
    }
    return true;
}

bool FAkUGCSceneRuntime::ApplyCommand(
    const FAkUGCCommand& Command,
    const FAkUGCPrefabRegistry& Registry,
    TSet<FGuid>& OutAttachmentUpdates,
    FString* OutError)
{
    switch (Command.Type)
    {
    case EAkUGCCommandType::AddEntity:
        if (!SpawnEntity(Command.Entity, Registry, OutError))
        {
            return false;
        }
        OutAttachmentUpdates.Add(Command.Entity.EntityId);
        return true;

    case EAkUGCCommandType::DeleteEntity:
    {
        AActor* Actor = FindActor(Command.EntityId);
        if (!Actor)
        {
            if (ExternallyDeletedEntityIds.Remove(Command.EntityId) > 0)
            {
                OutAttachmentUpdates.Remove(Command.EntityId);
                return true;
            }
            return Fail(OutError, TEXT("Cannot delete a runtime entity that does not exist."));
        }

        TArray<AActor*> AttachedActors;
        Actor->GetAttachedActors(AttachedActors);
        for (AActor* AttachedActor : AttachedActors)
        {
            if (const UAkUGCEntityBindingComponent* ChildBinding = FindBinding(AttachedActor))
            {
                OutAttachmentUpdates.Add(ChildBinding->EntityId);
            }
        }

        if (!RemoveEntity(Command.EntityId))
        {
            return Fail(OutError, TEXT("Failed to delete runtime entity."));
        }
        OutAttachmentUpdates.Remove(Command.EntityId);
        return true;
    }

    case EAkUGCCommandType::SetTransform:
    case EAkUGCCommandType::SetProperty:
    case EAkUGCCommandType::RemoveProperty:
    case EAkUGCCommandType::SetParent:
    {
        AActor* Actor = FindActor(Command.EntityId);
        UAkUGCEntityBindingComponent* Binding = FindBinding(Actor);
        if (!Binding)
        {
            return Fail(OutError, TEXT("Runtime entity binding does not exist."));
        }

        FAkUGCEntityRecord UpdatedRecord = Binding->SourceRecord;
        if (Command.Type == EAkUGCCommandType::SetTransform)
        {
            UpdatedRecord.Transform = Command.Transform;
        }
        else if (Command.Type == EAkUGCCommandType::SetParent)
        {
            UpdatedRecord.ParentEntityId = Command.ParentEntityId;
            OutAttachmentUpdates.Add(Command.EntityId);
        }
        else
        {
            FAkUGCComponentRecord* Component = UpdatedRecord.Components.FindByPredicate([&Command](const FAkUGCComponentRecord& Candidate)
            {
                return Candidate.TypeId == Command.ComponentTypeId;
            });
            if (!Component)
            {
                return Fail(OutError, TEXT("Runtime entity component does not exist."));
            }

            if (Command.Type == EAkUGCCommandType::SetProperty)
            {
                Component->Properties.Add(Command.PropertyId, Command.PropertyValue);
            }
            else if (Component->Properties.Remove(Command.PropertyId) == 0)
            {
                return Fail(OutError, TEXT("Runtime entity property does not exist."));
            }
        }

        Binding->ApplyRecord(UpdatedRecord);
        return true;
    }

    case EAkUGCCommandType::DuplicateEntity:
    {
        AActor* SourceActor = FindActor(Command.SourceEntityId);
        const UAkUGCEntityBindingComponent* SourceBinding = FindBinding(SourceActor);
        if (!SourceBinding)
        {
            return Fail(OutError, TEXT("Runtime duplicate source entity does not exist."));
        }

        FAkUGCEntityRecord Duplicate = SourceBinding->SourceRecord;
        Duplicate.EntityId = Command.EntityId;
        Duplicate.Transform = Command.Transform;
        if (!SpawnEntity(Duplicate, Registry, OutError))
        {
            return false;
        }
        OutAttachmentUpdates.Add(Duplicate.EntityId);
        return true;
    }

    case EAkUGCCommandType::AddLogicNode:
    case EAkUGCCommandType::DeleteLogicNode:
    case EAkUGCCommandType::ConnectLogicNode:
    case EAkUGCCommandType::DisconnectLogicNode:
    case EAkUGCCommandType::UpdateLogicNode:
    case EAkUGCCommandType::AddWave:
    case EAkUGCCommandType::UpdateWave:
    case EAkUGCCommandType::DeleteWave:
    case EAkUGCCommandType::MoveWave:
    case EAkUGCCommandType::SetRulesetSettings:
        return true;
    }

    return Fail(OutError, TEXT("Unsupported runtime command type."));
}

bool FAkUGCSceneRuntime::SpawnEntity(
    const FAkUGCEntityRecord& Entity,
    const FAkUGCPrefabRegistry& Registry,
    FString* OutError)
{
    UWorld* RuntimeWorld = World.Get();
    if (!RuntimeWorld)
    {
        return Fail(OutError, TEXT("Runtime world is not valid."));
    }
    if (Actors.Contains(Entity.EntityId))
    {
        return Fail(OutError, TEXT("Entity is already spawned."));
    }

    const FAkUGCPrefabDefinition* Definition = Registry.Find(Entity.PrefabId);
    if (!Definition)
    {
        return Fail(OutError, FString::Printf(TEXT("Prefab '%s' is not registered."), *Entity.PrefabId.ToString()));
    }

    UClass* ActorClass = AAkUGCRuntimeEntityActor::StaticClass();
    UStaticMesh* StaticMesh = nullptr;
    if (const FSoftObjectPath* AssetPath = Registry.ResolveAsset(Entity.PrefabId, GetCurrentPlatformVariant()))
    {
        if (AssetPath->IsValid())
        {
            UObject* Asset = AssetPath->TryLoad();
            if (!Asset)
            {
                return Fail(OutError, FString::Printf(TEXT("Failed to load prefab asset '%s'."), *AssetPath->ToString()));
            }
            if (UClass* LoadedClass = Cast<UClass>(Asset))
            {
                if (!LoadedClass->IsChildOf(AActor::StaticClass()))
                {
                    return Fail(OutError, TEXT("Prefab class asset must derive from AActor."));
                }
                ActorClass = LoadedClass;
            }
            else if (UStaticMesh* LoadedMesh = Cast<UStaticMesh>(Asset))
            {
                StaticMesh = LoadedMesh;
            }
            else
            {
                return Fail(OutError, TEXT("Prefab runtime asset must be an Actor class or StaticMesh."));
            }
        }
    }

    FActorSpawnParameters SpawnParameters;
    SpawnParameters.ObjectFlags |= RF_Transient;
    SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    SpawnParameters.Name = MakeUniqueObjectName(
        RuntimeWorld->PersistentLevel,
        ActorClass,
        FName(*FString::Printf(TEXT("UGC_%s"), *Entity.EntityId.ToString(EGuidFormats::Digits))));

    AActor* Actor = RuntimeWorld->SpawnActor<AActor>(ActorClass, Entity.Transform, SpawnParameters);
    if (!Actor)
    {
        return Fail(OutError, TEXT("Failed to spawn runtime actor."));
    }

    UAkUGCEntityBindingComponent* Binding = FindBinding(Actor);
    if (!Binding)
    {
        Binding = NewObject<UAkUGCEntityBindingComponent>(Actor, TEXT("UGCEntityBinding"), RF_Transient);
        Actor->AddInstanceComponent(Binding);
        Binding->RegisterComponent();
    }
    Binding->ApplyRecord(Entity);

    if (AAkUGCRuntimeEntityActor* RuntimeEntity = Cast<AAkUGCRuntimeEntityActor>(Actor))
    {
        RuntimeEntity->SetEntityIdentity(Entity.EntityId, Entity.PrefabId);
    }

    if (StaticMesh)
    {
        if (AAkUGCRuntimeEntityActor* RuntimeEntity = Cast<AAkUGCRuntimeEntityActor>(Actor))
        {
            RuntimeEntity->SetVisualMesh(StaticMesh);
        }
    }

    Actors.Add(Entity.EntityId, Actor);
    ExternallyDeletedEntityIds.Remove(Entity.EntityId);
    return true;
}

bool FAkUGCSceneRuntime::RefreshAttachments(const TSet<FGuid>& EntityIds, FString* OutError)
{
    TMap<FGuid, uint8> VisitStates;
    TArray<FGuid> OrderedEntityIds;
    TFunction<bool(const FGuid&)> Visit = [&](const FGuid& EntityId)
    {
        const uint8 State = VisitStates.FindRef(EntityId);
        if (State == 1)
        {
            return Fail(OutError, TEXT("Runtime parent hierarchy contains a cycle."));
        }
        if (State == 2)
        {
            return true;
        }

        AActor* Actor = FindActor(EntityId);
        const UAkUGCEntityBindingComponent* Binding = FindBinding(Actor);
        if (!Binding)
        {
            return Fail(OutError, TEXT("Expected runtime entity is missing while ordering attachments."));
        }

        VisitStates.Add(EntityId, 1);
        const FGuid ParentId = Binding->SourceRecord.ParentEntityId;
        if (ParentId.IsValid() && !Visit(ParentId))
        {
            return false;
        }
        VisitStates.Add(EntityId, 2);

        if (EntityIds.Contains(EntityId))
        {
            OrderedEntityIds.Add(EntityId);
        }
        return true;
    };

    TArray<FGuid> StableEntityIds = EntityIds.Array();
    StableEntityIds.Sort([](const FGuid& Left, const FGuid& Right)
    {
        return Left.ToString(EGuidFormats::Digits) < Right.ToString(EGuidFormats::Digits);
    });

    for (const FGuid& EntityId : StableEntityIds)
    {
        if (!Visit(EntityId))
        {
            return false;
        }
    }

    for (const FGuid& EntityId : OrderedEntityIds)
    {
        if (!RefreshAttachment(EntityId, OutError))
        {
            return false;
        }
    }
    return true;
}

bool FAkUGCSceneRuntime::RefreshAttachment(const FGuid& EntityId, FString* OutError)
{
    AActor* Actor = FindActor(EntityId);
    if (!Actor)
    {
        return Fail(OutError, TEXT("Expected runtime entity is missing while refreshing attachment."));
    }

    const UAkUGCEntityBindingComponent* Binding = FindBinding(Actor);
    if (!Binding)
    {
        return Fail(OutError, TEXT("Runtime entity binding does not exist while refreshing attachment."));
    }

    Actor->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
    if (!Binding->SourceRecord.ParentEntityId.IsValid())
    {
        return true;
    }

    AActor* Parent = FindActor(Binding->SourceRecord.ParentEntityId);
    if (!Parent)
    {
        return Fail(OutError, TEXT("Runtime parent entity does not exist."));
    }
    if (Parent == Actor)
    {
        return Fail(OutError, TEXT("Runtime entity cannot be parented to itself."));
    }

    if (!Actor->AttachToActor(Parent, FAttachmentTransformRules::KeepWorldTransform))
    {
        return Fail(OutError, TEXT("Unreal rejected the runtime entity attachment."));
    }
    return true;
}

bool FAkUGCSceneRuntime::AttachParents(const FAkUGCSceneDocument& Scene, FString* OutError)
{
    TSet<FGuid> EntityIds;
    for (const FAkUGCEntityRecord& Entity : Scene.Entities)
    {
        EntityIds.Add(Entity.EntityId);
    }
    return RefreshAttachments(EntityIds, OutError);
}

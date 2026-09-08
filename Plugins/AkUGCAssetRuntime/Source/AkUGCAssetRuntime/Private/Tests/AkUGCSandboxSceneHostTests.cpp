#include "Misc/AutomationTest.h"

#include "AkUGCSandbox.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Prefab/AkUGCOfficialPrefabCatalog.h"
#include "Prefab/AkUGCPrefabRegistry.h"
#include "Scene/AkUGCSandboxSceneHost.h"
#include "Scene/AkUGCSceneRuntime.h"
#include "Session/AkUGCDocumentRuntimeSession.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAkUGCSandboxSceneHostTest,
	"AkUGC.Runtime.Sandbox.SceneHostQueriesAndDamages",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCSandboxSceneHostTest::RunTest(const FString& Parameters)
{
	if (!GEngine)
	{
		AddError(TEXT("Engine is not available."));
		return false;
	}

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

	FAkUGCPrefabRegistry Registry;
	FString Error;
	TestTrue(TEXT("Official prefab catalog registers"),
		FAkUGCOfficialPrefabCatalog::RegisterTowerDefense(Registry, &Error));

	FAkUGCProjectDocument Document;
	Document.Manifest.ProjectId = FGuid::NewGuid();
	Document.Manifest.DisplayName = TEXT("Sandbox Scene Host Integration");
	Document.Manifest.TemplateId = TEXT("official.tower_defense");
	FAkUGCSceneDocument& Scene = Document.Scenes.AddDefaulted_GetRef();
	Scene.SceneId = FGuid::NewGuid();
	Scene.DisplayName = TEXT("Main");

	const FGuid SpawnPointId = FGuid::NewGuid();
	FAkUGCEntityRecord SpawnPoint;
	TestTrue(TEXT("Enemy Spawn record is created"), Registry.CreateEntityRecord(
		TEXT("official.gameplay.enemy_spawn"),
		SpawnPointId,
		FTransform(FVector(250.0, 50.0, 0.0)),
		SpawnPoint,
		&Error));
	FAkUGCComponentRecord* SpawnConfig = SpawnPoint.Components.FindByPredicate([](const FAkUGCComponentRecord& Component)
	{
		return Component.TypeId == TEXT("tower_defense.spawn");
	});
	TestNotNull(TEXT("Enemy Spawn exposes batch configuration"), SpawnConfig);
	if (!SpawnConfig)
	{
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
		return false;
	}
	SpawnConfig->Properties.FindChecked(TEXT("enemyCount")).IntegerValue = 3;
	SpawnConfig->Properties.FindChecked(TEXT("spawnInterval")).NumberValue = 0.25;
	Scene.Entities.Add(SpawnPoint);
	for (int32 WaveIndex = 0; WaveIndex < AkUGCTowerDefenseRulesetLimits::RequiredWaveCount; ++WaveIndex)
	{
		FAkUGCTowerDefenseWave& Wave = Scene.Ruleset.Waves.AddDefaulted_GetRef();
		Wave.WaveId = FGuid::NewGuid();
		Wave.SpawnPointEntityId = SpawnPointId;
		Wave.StartDelaySeconds = WaveIndex;
	}

	FAkUGCEntityRecord FirstPathNode;
	FAkUGCEntityRecord SecondPathNode;
	TestTrue(TEXT("First gameplay path node is created"), Registry.CreateEntityRecord(
		TEXT("official.gameplay.path_node"),
		FGuid::NewGuid(),
		FTransform(FVector(500.0, 50.0, 0.0)),
		FirstPathNode,
		&Error));
	TestTrue(TEXT("Second gameplay path node is created"), Registry.CreateEntityRecord(
		TEXT("official.gameplay.path_node"),
		FGuid::NewGuid(),
		FTransform(FVector(1000.0, 50.0, 0.0)),
		SecondPathNode,
		&Error));
	FirstPathNode.Components[0].Properties.FindChecked(TEXT("order")).IntegerValue = 0;
	SecondPathNode.Components[0].Properties.FindChecked(TEXT("order")).IntegerValue = 1;
	Scene.Entities.Add(FirstPathNode);
	Scene.Entities.Add(SecondPathNode);

	const FGuid BaseEntityId = FGuid::NewGuid();
	const FGuid GoalEntityId = FGuid::NewGuid();
	FAkUGCEntityRecord BaseEntity;
	FAkUGCEntityRecord GoalEntity;
	TestTrue(TEXT("Tower defense Base is created"), Registry.CreateEntityRecord(
		TEXT("official.gameplay.base"),
		BaseEntityId,
		FTransform(FVector(1100.0, 50.0, 0.0)),
		BaseEntity,
		&Error));
	TestTrue(TEXT("Tower defense Goal is created"), Registry.CreateEntityRecord(
		TEXT("official.gameplay.goal"),
		GoalEntityId,
		FTransform(FVector(1000.0, 50.0, 0.0)),
		GoalEntity,
		&Error));
	BaseEntity.Components[0].Properties.FindChecked(TEXT("maxHealth")).NumberValue = 25.0;
	GoalEntity.Components[0].Properties.FindChecked(TEXT("baseDamage")).NumberValue = 99.0;
	Scene.Entities.Add(BaseEntity);
	Scene.Entities.Add(GoalEntity);

	FAkUGCLogicNode Start;
	Start.NodeId = FGuid::NewGuid();
	Start.Type = EAkUGCLogicNodeType::GameStart;
	FAkUGCLogicNode WaveStart;
	WaveStart.NodeId = FGuid::NewGuid();
	WaveStart.Type = EAkUGCLogicNodeType::WaveStart;
	Scene.LogicGraph.Nodes = {Start, WaveStart};

	{
		FAkUGCSceneRuntime Runtime(World);
		FAkUGCDocumentRuntimeSession Session(Runtime, Registry, Scene.SceneId, EAkUGCRuntimeSessionMode::PlayAuthority);
		const auto InitResult = Session.Initialize(Document);
		TestTrue(TEXT("Play Authority initializes tower defense scene"), InitResult.bSucceeded);
		if (!InitResult.bSucceeded)
		{
			AddError(FString::Printf(TEXT("Play Authority initialization failed: %s: %s"), *InitResult.ErrorPath, *InitResult.ErrorMessage));
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
			return false;
		}
		TestEqual(TEXT("Base health initializes to authored maximum"), Runtime.GetBaseCurrentHealth(), 25.0);

		const TSharedPtr<FAkUGCSandboxSceneHost> Host = MakeShared<FAkUGCSandboxSceneHost>(Runtime, Registry);
		FAkUGCSandbox Sandbox;
		Sandbox.Initialize(FAkUGCSandboxConfig(), nullptr, Host);

		const FString BaseIdStr = BaseEntityId.ToString();
		const FString GoalIdStr = GoalEntityId.ToString();
		const FString SpawnPointIdStr = SpawnPointId.ToString();

		const FString Script = FString::Printf(TEXT(
			"local cur, max = ugc.get_health('%s')\n"
			"assert(cur == 25 and max == 25, 'base health init mismatch')\n"
			"assert(ugc.get_health('not-a-guid') == nil, 'invalid id returns nil')\n"
			"local applied, after = ugc.apply_damage('%s', '%s', 10)\n"
			"assert(applied == 10 and after == 15, 'base damage mismatch')\n"
			"local ws = ugc.get_wave_state()\n"
			"assert(ws ~= nil, 'wave state must be available')\n"
			"assert(ws.total_waves == 3, 'wave count mismatch')\n"
			"local spawned = ugc.spawn('official.unit.basic_enemy', '%s')\n"
			"assert(spawned ~= nil and #spawned > 0, 'spawn returns entity id')\n"
			"local s_cur, s_max = ugc.get_health(spawned)\n"
			"assert(s_cur == 100 and s_max == 100, 'spawned enemy health mismatch')\n"),
			*BaseIdStr, *GoalIdStr, *BaseIdStr, *SpawnPointIdStr);

		const FAkUGCSandboxResult Result = Sandbox.RunScript(Script);
		if (Result.Status != EAkUGCSandboxStatus::Success)
		{
			AddError(FString::Printf(TEXT("Sandbox script error: %s"), *Result.ErrorMessage));
		}
		TestEqual(TEXT("Sandbox script queries/damages/spawns and reads wave state via host"), Result.Status, EAkUGCSandboxStatus::Success);

		TestEqual(TEXT("Base health reflects scripted damage"), Runtime.GetBaseCurrentHealth(), 15.0);
	}

	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

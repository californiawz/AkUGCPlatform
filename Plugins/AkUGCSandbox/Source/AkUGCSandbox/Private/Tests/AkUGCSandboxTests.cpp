#include "Misc/AutomationTest.h"
#include "AkUGCSandbox.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAkUGCSandboxRunTest,
	"AkUGC.Sandbox.VM.RunsScript",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCSandboxRunTest::RunTest(const FString& Parameters)
{
	FAkUGCSandbox Sandbox;
	FString Error;
	TestTrue(*Error, Sandbox.Initialize(FAkUGCSandboxConfig(), &Error));

	const FString Script = TEXT(
		"local sum = 0\n"
		"for i = 1, 100 do sum = sum + i end\n"
		"assert(sum == 5050, 'unexpected sum')\n");

	const FAkUGCSandboxResult Result = Sandbox.RunScript(Script);
	TestEqual(TEXT("script runs successfully"), Result.Status, EAkUGCSandboxStatus::Success);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAkUGCSandboxForbiddenTest,
	"AkUGC.Sandbox.VM.ForbiddenLibraries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCSandboxForbiddenTest::RunTest(const FString& Parameters)
{
	FAkUGCSandbox Sandbox;
	Sandbox.Initialize(FAkUGCSandboxConfig());

	const FString Script = TEXT(
		"assert(type(io) == 'nil', 'io must not exist')\n"
		"assert(type(os) == 'nil', 'os must not exist')\n"
		"assert(type(debug) == 'nil', 'debug must not exist')\n"
		"assert(type(package) == 'nil', 'package must not exist')\n"
		"assert(type(require) == 'nil', 'require must not exist')\n"
		"assert(type(dofile) == 'nil', 'dofile must not exist')\n"
		"assert(type(loadfile) == 'nil', 'loadfile must not exist')\n"
		"assert(type(load) == 'nil', 'load must not exist')\n");

	const FAkUGCSandboxResult Result = Sandbox.RunScript(Script);
	TestEqual(TEXT("forbidden libraries are absent"), Result.Status, EAkUGCSandboxStatus::Success);
	TestTrue(TEXT("no error message on success"), Result.ErrorMessage.IsEmpty());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAkUGCSandboxSafeLibsTest,
	"AkUGC.Sandbox.VM.SafeLibraries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCSandboxSafeLibsTest::RunTest(const FString& Parameters)
{
	FAkUGCSandbox Sandbox;
	Sandbox.Initialize(FAkUGCSandboxConfig());

	const FString Script = TEXT(
		"assert(type(table) == 'table')\n"
		"assert(type(string) == 'table')\n"
		"assert(type(math) == 'table')\n"
		"assert(type(utf8) == 'table')\n"
		"assert(string.upper('abc') == 'ABC')\n"
		"assert(math.max(1, 2) == 2)\n"
		"assert(table.concat({1, 2, 3}, ',') == '1,2,3')\n"
		"assert((0xF & 0x3) == 0x3)\n");

	const FAkUGCSandboxResult Result = Sandbox.RunScript(Script);
	TestEqual(TEXT("safe libraries are available"), Result.Status, EAkUGCSandboxStatus::Success);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAkUGCSandboxInstructionLimitTest,
	"AkUGC.Sandbox.VM.InstructionLimit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCSandboxInstructionLimitTest::RunTest(const FString& Parameters)
{
	FAkUGCSandboxConfig Config;
	Config.MaxInstructionCount = 100000;
	Config.InstructionCheckInterval = 1000;

	FAkUGCSandbox Sandbox;
	Sandbox.Initialize(Config);

	const FAkUGCSandboxResult Result = Sandbox.RunScript(TEXT("while true do end\n"));
	TestEqual(TEXT("infinite loop is terminated by instruction limit"), Result.Status, EAkUGCSandboxStatus::InstructionLimitExceeded);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAkUGCSandboxIsolationTest,
	"AkUGC.Sandbox.VM.Isolation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCSandboxIsolationTest::RunTest(const FString& Parameters)
{
	FAkUGCSandbox SandboxA;
	FAkUGCSandbox SandboxB;
	SandboxA.Initialize(FAkUGCSandboxConfig());
	SandboxB.Initialize(FAkUGCSandboxConfig());

	// A 写入全局变量。
	SandboxA.RunScript(TEXT("shared_var = 42\n"));

	// B 中该变量应不存在（VM 隔离）。
	const FAkUGCSandboxResult ResultB = SandboxB.RunScript(TEXT("assert(shared_var == nil, 'VM is not isolated')\n"));
	TestEqual(TEXT("VM B does not see VM A globals"), ResultB.Status, EAkUGCSandboxStatus::Success);

	// A 中该变量应仍然存在。
	const FAkUGCSandboxResult ResultA = SandboxA.RunScript(TEXT("assert(shared_var == 42, 'VM A lost its global')\n"));
	TestEqual(TEXT("VM A retains its globals"), ResultA.Status, EAkUGCSandboxStatus::Success);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAkUGCSandboxMemoryLimitTest,
	"AkUGC.Sandbox.VM.MemoryLimit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCSandboxMemoryLimitTest::RunTest(const FString& Parameters)
{
	FAkUGCSandboxConfig Config;
	Config.MaxMemoryBytes = 1024 * 1024;  // 1MB

	FAkUGCSandbox Sandbox;
	Sandbox.Initialize(Config);

	// 尝试分配远超配额的内存。
	const FString Script = TEXT("local s = string.rep('x', 64 * 1024 * 1024)\n");
	const FAkUGCSandboxResult Result = Sandbox.RunScript(Script);
	TestNotEqual(TEXT("excessive allocation is rejected"), Result.Status, EAkUGCSandboxStatus::Success);

	return true;
}

namespace
{
	/** 记录受控消息、实体查询与伤害的测试宿主。 */
	class FMockSandboxHost : public IAkUGCSandboxHost
	{
	public:
		virtual void EmitMessage(const FString& Message) override
		{
			Messages.Add(Message);
		}

		virtual bool QueryEntityHealth(const FString& EntityId, double& OutCurrent, double& OutMaximum) override
		{
			const double* Current = HealthByEntity.Find(EntityId);
			const double* Maximum = MaxHealthByEntity.Find(EntityId);
			if (!Current || !Maximum)
			{
				return false;
			}
			OutCurrent = *Current;
			OutMaximum = *Maximum;
			return true;
		}

		virtual bool ApplyDamage(
			const FString& SourceEntityId,
			const FString& TargetEntityId,
			double Damage,
			double& OutAppliedDamage,
			double& OutHealthAfter,
			bool& OutKilled,
			FString& OutError) override
		{
			double* Current = HealthByEntity.Find(TargetEntityId);
			if (!Current)
			{
				OutError = TEXT("unknown target entity");
				return false;
			}
			const double Previous = *Current;
			*Current = (Previous > Damage) ? (Previous - Damage) : 0.0;
			OutAppliedDamage = Previous - *Current;
			OutHealthAfter = *Current;
			OutKilled = *Current <= 0.0;
			LastDamage = FAkUGCMockDamage{SourceEntityId, TargetEntityId, Damage};
			return true;
		}

		virtual bool SpawnEntity(
			const FString& PrefabId,
			const FString& AnchorEntityId,
			FString& OutEntityId,
			FString& OutError) override
		{
			OutEntityId = FString::Printf(TEXT("spawned-%d"), ++SpawnCounter);
			SpawnedPrefabs.Add(FString::Printf(TEXT("%s@%s"), *PrefabId, *AnchorEntityId));
			return true;
		}

		virtual bool QueryWaveState(FAkUGCSandboxWaveState& OutState) override
		{
			OutState = WaveState;
			return bHasWaveState;
		}

		struct FAkUGCMockDamage
		{
			FString Source;
			FString Target;
			double Damage = 0.0;
		};

		TArray<FString> Messages;
		TMap<FString, double> HealthByEntity;
		TMap<FString, double> MaxHealthByEntity;
		FAkUGCMockDamage LastDamage;
		TArray<FString> SpawnedPrefabs;
		FAkUGCSandboxWaveState WaveState;
		bool bHasWaveState = false;
		int32 SpawnCounter = 0;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAkUGCSandboxControlledApiTest,
	"AkUGC.Sandbox.VM.ControlledApiMessage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCSandboxControlledApiTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FMockSandboxHost> Host = MakeShared<FMockSandboxHost>();

	FAkUGCSandbox Sandbox;
	Sandbox.Initialize(FAkUGCSandboxConfig(), nullptr, Host);

	const FString Script = TEXT("ugc.message('hello sandbox')\n");
	const FAkUGCSandboxResult Result = Sandbox.RunScript(Script);
	TestEqual(TEXT("script runs successfully"), Result.Status, EAkUGCSandboxStatus::Success);
	TestEqual(TEXT("one message forwarded to host"), Host->Messages.Num(), 1);
	if (Host->Messages.Num() == 1)
	{
		TestEqual(TEXT("message content forwarded intact"), Host->Messages[0], TEXT("hello sandbox"));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAkUGCSandboxNoHostApiAbsentTest,
	"AkUGC.Sandbox.VM.NoHostApiAbsent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCSandboxNoHostApiAbsentTest::RunTest(const FString& Parameters)
{
	FAkUGCSandbox Sandbox;
	Sandbox.Initialize(FAkUGCSandboxConfig());

	// 未注入宿主时，ugc 命名空间必须不存在。
	const FString Script = TEXT("assert(ugc == nil, 'ugc must be absent without a host')\n");
	const FAkUGCSandboxResult Result = Sandbox.RunScript(Script);
	TestEqual(TEXT("ugc is absent without a host"), Result.Status, EAkUGCSandboxStatus::Success);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAkUGCSandboxCallDepthLimitTest,
	"AkUGC.Sandbox.VM.CallDepthLimit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCSandboxCallDepthLimitTest::RunTest(const FString& Parameters)
{
	FAkUGCSandboxConfig Config;
	Config.MaxCallDepth = 32;

	FAkUGCSandbox Sandbox;
	Sandbox.Initialize(Config);

	// 非尾调用的无终止递归：每次递归都保留上一帧，调用深度持续增长。
	const FString Script = TEXT(
		"local function recurse()\n"
		"    local x = recurse()\n"
		"    return x\n"
		"end\n"
		"recurse()\n");

	const FAkUGCSandboxResult Result = Sandbox.RunScript(Script);
	TestEqual(TEXT("deep recursion is terminated by call depth limit"), Result.Status, EAkUGCSandboxStatus::CallDepthExceeded);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAkUGCSandboxGetHealthTest,
	"AkUGC.Sandbox.VM.ControlledApiGetHealth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCSandboxGetHealthTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FMockSandboxHost> Host = MakeShared<FMockSandboxHost>();
	Host->HealthByEntity.Add(TEXT("entity-1"), 70.0);
	Host->MaxHealthByEntity.Add(TEXT("entity-1"), 100.0);

	FAkUGCSandbox Sandbox;
	Sandbox.Initialize(FAkUGCSandboxConfig(), nullptr, Host);

	const FString Script = TEXT(
		"local cur, max = ugc.get_health('entity-1')\n"
		"assert(cur == 70 and max == 100, 'health mismatch')\n"
		"assert(ugc.get_health('entity-missing') == nil, 'missing must be nil')\n");

	const FAkUGCSandboxResult Result = Sandbox.RunScript(Script);
	TestEqual(TEXT("get_health queries and returns values"), Result.Status, EAkUGCSandboxStatus::Success);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAkUGCSandboxApplyDamageTest,
	"AkUGC.Sandbox.VM.ControlledApiApplyDamage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCSandboxApplyDamageTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FMockSandboxHost> Host = MakeShared<FMockSandboxHost>();
	Host->HealthByEntity.Add(TEXT("enemy-1"), 100.0);
	Host->MaxHealthByEntity.Add(TEXT("enemy-1"), 100.0);

	FAkUGCSandbox Sandbox;
	Sandbox.Initialize(FAkUGCSandboxConfig(), nullptr, Host);

	const FString Script = TEXT(
		"local applied, after = ugc.apply_damage('tower-1', 'enemy-1', 30)\n"
		"assert(applied == 30 and after == 70, 'damage mismatch')\n");

	const FAkUGCSandboxResult Result = Sandbox.RunScript(Script);
	TestEqual(TEXT("apply_damage forwards to host"), Result.Status, EAkUGCSandboxStatus::Success);
	TestEqual(TEXT("host received target id"), Host->LastDamage.Target, TEXT("enemy-1"));
	TestEqual(TEXT("host received source id"), Host->LastDamage.Source, TEXT("tower-1"));
	TestEqual(TEXT("host received damage amount"), Host->LastDamage.Damage, 30.0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAkUGCSandboxSpawnTest,
	"AkUGC.Sandbox.VM.ControlledApiSpawn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCSandboxSpawnTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FMockSandboxHost> Host = MakeShared<FMockSandboxHost>();

	FAkUGCSandbox Sandbox;
	Sandbox.Initialize(FAkUGCSandboxConfig(), nullptr, Host);

	const FString Script = TEXT(
		"local id = ugc.spawn('prefab-1', 'anchor-1')\n"
		"assert(type(id) == 'string' and #id > 0, 'spawn must return entity id')\n");

	const FAkUGCSandboxResult Result = Sandbox.RunScript(Script);
	TestEqual(TEXT("spawn forwards to host"), Result.Status, EAkUGCSandboxStatus::Success);
	TestEqual(TEXT("host received one spawn request"), Host->SpawnedPrefabs.Num(), 1);
	if (Host->SpawnedPrefabs.Num() == 1)
	{
		TestEqual(TEXT("host received prefab and anchor"), Host->SpawnedPrefabs[0], TEXT("prefab-1@anchor-1"));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAkUGCSandboxGetWaveStateTest,
	"AkUGC.Sandbox.VM.ControlledApiGetWaveState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCSandboxGetWaveStateTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FMockSandboxHost> Host = MakeShared<FMockSandboxHost>();
	Host->bHasWaveState = true;
	Host->WaveState.WaveIndex = 1;
	Host->WaveState.TotalWaves = 3;
	Host->WaveState.State = 2;
	Host->WaveState.Result = 0;
	Host->WaveState.SecondsUntilNextBoundary = 1.5;

	FAkUGCSandbox Sandbox;
	Sandbox.Initialize(FAkUGCSandboxConfig(), nullptr, Host);

	const FString Script = TEXT(
		"local ws = ugc.get_wave_state()\n"
		"assert(ws ~= nil, 'wave state must be a table')\n"
		"assert(ws.wave_index == 1, 'wave index mismatch')\n"
		"assert(ws.total_waves == 3, 'total waves mismatch')\n"
		"assert(ws.state == 2, 'state mismatch')\n"
		"assert(ws.result == 0, 'result mismatch')\n"
		"assert(ws.seconds_until_boundary == 1.5, 'boundary mismatch')\n");

	const FAkUGCSandboxResult Result = Sandbox.RunScript(Script);
	TestEqual(TEXT("get_wave_state returns mapped snapshot"), Result.Status, EAkUGCSandboxStatus::Success);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAkUGCSandboxTimerAfterTest,
	"AkUGC.Sandbox.VM.ControlledApiTimerAfter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCSandboxTimerAfterTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FMockSandboxHost> Host = MakeShared<FMockSandboxHost>();

	FAkUGCSandbox Sandbox;
	Sandbox.Initialize(FAkUGCSandboxConfig(), nullptr, Host);

	const FString Script = TEXT(
		"local id = ugc.timer_after(0.5, function() ugc.message('timer fired') end)\n"
		"assert(type(id) == 'number' and id >= 1, 'timer id must be positive')\n");

	const FAkUGCSandboxResult Result = Sandbox.RunScript(Script);
	TestEqual(TEXT("timer registration succeeds"), Result.Status, EAkUGCSandboxStatus::Success);
	TestEqual(TEXT("no message before timer fires"), Host->Messages.Num(), 0);

	const int32 Fired = Sandbox.AdvanceTimers(0.6);
	TestEqual(TEXT("one timer fired after advancing"), Fired, 1);
	TestEqual(TEXT("callback forwarded message"), Host->Messages.Num(), 1);
	if (Host->Messages.Num() == 1)
	{
		TestEqual(TEXT("callback message content"), Host->Messages[0], TEXT("timer fired"));
	}

	TestEqual(TEXT("timer does not refire"), Sandbox.AdvanceTimers(0.6), 0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

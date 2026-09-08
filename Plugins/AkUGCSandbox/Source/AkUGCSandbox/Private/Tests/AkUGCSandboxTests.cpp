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

#endif // WITH_DEV_AUTOMATION_TESTS

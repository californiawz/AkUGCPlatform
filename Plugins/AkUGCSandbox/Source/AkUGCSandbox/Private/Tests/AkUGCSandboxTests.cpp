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
	/** 记录受控消息的测试宿主。 */
	class FMockSandboxHost : public IAkUGCSandboxHost
	{
	public:
		virtual void EmitMessage(const FString& Message) override
		{
			Messages.Add(Message);
		}

		TArray<FString> Messages;
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

#endif // WITH_DEV_AUTOMATION_TESTS

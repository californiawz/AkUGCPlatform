#include "AkUGCSandbox.h"

#include "Containers/StringConv.h"
#include "HAL/PlatformTime.h"

#include "Core/LuaVirtualMachine.h"
#include "Core/LuaCallScope.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

using namespace slua;

/** 沙箱 VM 内部状态。 */
struct FAkUGCSandboxImpl
{
	FAkUGCSandboxConfig Config;

	// 复用 AkLuaRuntime 的 FLuaVirtualMachine 封装 VM 生命周期；
	// 沙盒只在 VM 之上额外创建独立 env，不再自行 new LuaState。
	TSharedPtr<FLuaVirtualMachine> VirtualMachine;
	lua_State* State = nullptr;

	int64 InstructionsExecuted = 0;
	int64 BaseMemoryBytes = 0;
	int32 HookInterval = 1000;
	bool bInstructionLimitExceeded = false;
	bool bMemoryLimitExceeded = false;
	bool bTimeout = false;
	double RunStartTime = 0.0;
};

namespace
{
	const char* SandboxImplRegistryKey = "AkUGCSandboxImpl";
	const char* SandboxEnvRegistryKey = "AkUGCSandboxEnv";

	/** 查询当前 VM 已用内存（字节）。 */
	int64 GetLuaMemoryBytes(lua_State* L)
	{
		const int KB = lua_gc(L, LUA_GCCOUNT, 0);
		const int B = lua_gc(L, LUA_GCCOUNTB, 0);
		return static_cast<int64>(KB) * 1024 + B;
	}

	/** 指令/超时/内存 hook：累计指令并检查三类配额，超限即终止。 */
	void SandboxCountHook(lua_State* L, lua_Debug* /*Ar*/)
	{
		lua_getfield(L, LUA_REGISTRYINDEX, SandboxImplRegistryKey);
		FAkUGCSandboxImpl* Impl = static_cast<FAkUGCSandboxImpl*>(lua_touserdata(L, -1));
		lua_pop(L, 1);
		if (!Impl)
		{
			return;
		}

		Impl->InstructionsExecuted += Impl->HookInterval;

		if (Impl->Config.MaxInstructionCount > 0 && Impl->InstructionsExecuted >= Impl->Config.MaxInstructionCount)
		{
			Impl->bInstructionLimitExceeded = true;
			luaL_error(L, "instruction limit exceeded");
		}

		if (Impl->Config.MaxRunTimeSeconds > 0.0)
		{
			const double Now = FPlatformTime::Seconds();
			if (Now - Impl->RunStartTime >= Impl->Config.MaxRunTimeSeconds)
			{
				Impl->bTimeout = true;
				luaL_error(L, "time limit exceeded");
			}
		}

		if (Impl->Config.MaxMemoryBytes > 0)
		{
			const int64 Used = GetLuaMemoryBytes(L) - Impl->BaseMemoryBytes;
			if (Used > Impl->Config.MaxMemoryBytes)
			{
				Impl->bMemoryLimitExceeded = true;
				luaL_error(L, "memory limit exceeded");
			}
		}
	}

	/**
	 * 创建沙盒独立环境表并存入注册表。
	 *
	 * 沙盒隔离不侵入宿主 _G，而是为脚本额外创建一个独立 env：
	 *   - sandbox_env 为空表，作为脚本的 _ENV，脚本全局读写均落在该表，实例间隔离；
	 *   - sandbox_env 的 __index 指向安全库白名单，脚本仅能访问白名单内的安全库；
	 *   - 宿主 _G 保持完整，AkLuaRuntime/slua 内部机制不受影响。
	 */
	void CreateSandboxEnv(lua_State* L)
	{
		// 安全全局白名单（base 精简子集 + 安全标准库）。
		static const char* SafeGlobalNames[] = {
			"assert", "error", "ipairs", "next", "pairs", "pcall", "xpcall",
			"select", "tonumber", "tostring", "type",
			"rawequal", "rawget", "rawlen", "rawset",
			"print",
			"table", "string", "math", "utf8",
		};

		// 1) 安全库表：从宿主 _G 引用白名单全局。
		lua_newtable(L);
		const int SafeGlobals = lua_gettop(L);
		for (const char* Name : SafeGlobalNames)
		{
			lua_getglobal(L, Name);
			lua_setfield(L, SafeGlobals, Name);
		}

		// 2) 沙盒 env 表（脚本的 _ENV）。
		lua_newtable(L);
		const int Env = lua_gettop(L);

		// 3) env 元表：__index = 安全库表。
		lua_newtable(L);
		lua_pushvalue(L, SafeGlobals);
		lua_setfield(L, -2, "__index");
		lua_setmetatable(L, Env);

		// 清理 safe_globals（已通过 env 元表被引用），仅留 env 在栈顶。
		lua_remove(L, SafeGlobals);

		// 存入注册表供 RunScript 使用。
		lua_setfield(L, LUA_REGISTRYINDEX, SandboxEnvRegistryKey);
	}

	/** 将栈顶 chunk 的 _ENV 上值替换为沙盒 env（无 _ENV 上值时弹出多余值）。 */
	void SetChunkEnv(lua_State* L)
	{
		lua_getfield(L, LUA_REGISTRYINDEX, SandboxEnvRegistryKey);
		if (lua_setupvalue(L, -2, 1) == nullptr)
		{
			lua_pop(L, 1);
		}
	}

	/** 弹出栈顶字符串并返回。 */
	FString PopTopString(lua_State* L)
	{
		size_t Length = 0;
		const char* Str = lua_tolstring(L, -1, &Length);
		FString Result = (Str != nullptr) ? UTF8_TO_TCHAR(Str) : FString();
		lua_pop(L, 1);
		return Result;
	}
}

FAkUGCSandbox::FAkUGCSandbox()
	: Impl(MakeUnique<FAkUGCSandboxImpl>())
{
}

FAkUGCSandbox::~FAkUGCSandbox()
{
	Shutdown();
}

bool FAkUGCSandbox::Initialize(const FAkUGCSandboxConfig& InConfig, FString* OutError)
{
	Shutdown();

	Impl->Config = InConfig;
	Impl->InstructionsExecuted = 0;
	Impl->BaseMemoryBytes = 0;
	Impl->bInstructionLimitExceeded = false;
	Impl->bMemoryLimitExceeded = false;
	Impl->bTimeout = false;

	// 复用 AkLuaRuntime 的 FLuaVirtualMachine 创建并初始化 Lua VM。
	Impl->VirtualMachine = MakeShared<FLuaVirtualMachine>();
	if (!Impl->VirtualMachine->Initialize(TEXT("AkUGCSandbox"), nullptr))
	{
		Impl->VirtualMachine.Reset();
		if (OutError)
		{
			*OutError = TEXT("failed to create Lua virtual machine via AkLuaRuntime");
		}
		return false;
	}
	Impl->State = Impl->VirtualMachine->GetRawState();

	// 创建独立沙盒 env（不侵入宿主 _G）。
	CreateSandboxEnv(Impl->State);

	// 记录基线内存，作为脚本增量配额基准（slua 初始化本身占用较大）。
	Impl->BaseMemoryBytes = GetLuaMemoryBytes(Impl->State);

	// 将内部状态指针存入注册表，供 hook 回调访问。
	lua_pushlightuserdata(Impl->State, Impl.Get());
	lua_setfield(Impl->State, LUA_REGISTRYINDEX, SandboxImplRegistryKey);

	// 开启指令/超时/内存 hook。
	const bool bNeedInstructionHook = (InConfig.MaxInstructionCount > 0) || (InConfig.MaxRunTimeSeconds > 0.0);
	const bool bNeedMemoryHook = (InConfig.MaxMemoryBytes > 0);
	if (bNeedInstructionHook || bNeedMemoryHook)
	{
		int32 Interval = (InConfig.InstructionCheckInterval > 0) ? InConfig.InstructionCheckInterval : 1000;
		if (bNeedMemoryHook)
		{
			// 内存检查需要较细粒度，尽量及时拦截循环中的累计分配。
			Interval = (Interval < 100) ? Interval : 100;
		}
		Impl->HookInterval = Interval;
		lua_sethook(Impl->State, &SandboxCountHook, LUA_MASKCOUNT, Interval);
	}

	return true;
}

void FAkUGCSandbox::Shutdown()
{
	// 交由 FLuaVirtualMachine::Shutdown 完成 VM 关闭与资源释放（幂等）。
	if (Impl->VirtualMachine)
	{
		Impl->VirtualMachine->Shutdown();
		Impl->VirtualMachine.Reset();
	}
	Impl->State = nullptr;
	Impl->BaseMemoryBytes = 0;
}

bool FAkUGCSandbox::IsInitialized() const
{
	return Impl->State != nullptr;
}

FAkUGCSandboxResult FAkUGCSandbox::RunScript(const FString& Source, const FString& ChunkName)
{
	FAkUGCSandboxResult Result;
	if (!Impl->State)
	{
		Result.Status = EAkUGCSandboxStatus::RuntimeError;
		Result.ErrorMessage = TEXT("sandbox is not initialized");
		return Result;
	}

	lua_State* L = Impl->State;
	FLuaCallScope Scope(L);

	// 编译源码。
	const auto SourceAnsi = StringCast<ANSICHAR>(*Source, Source.Len());
	const auto ChunkNameAnsi = StringCast<ANSICHAR>(*ChunkName, ChunkName.Len());
	const int LoadStatus = luaL_loadbuffer(L, SourceAnsi.Get(), SourceAnsi.Length(), ChunkNameAnsi.Get());
	if (LoadStatus != LUA_OK)
	{
		Result.Status = EAkUGCSandboxStatus::CompileError;
		Result.ErrorMessage = PopTopString(L);
		return Result;
	}

	// 将 chunk 的 _ENV 指向沙盒独立环境表。
	SetChunkEnv(L);

	// 运行。
	Impl->InstructionsExecuted = 0;
	Impl->bInstructionLimitExceeded = false;
	Impl->bMemoryLimitExceeded = false;
	Impl->bTimeout = false;
	Impl->RunStartTime = FPlatformTime::Seconds();

	const int RunStatus = lua_pcall(L, 0, 0, 0);
	if (RunStatus == LUA_OK)
	{
		// slua 未提供可注入的硬内存分配器，单次大分配（如 string.rep）无法被
		// hook 打断，只能在运行结束后检测增量内存是否越界。
		if (Impl->Config.MaxMemoryBytes > 0)
		{
			const int64 Used = GetLuaMemoryBytes(L) - Impl->BaseMemoryBytes;
			if (Used > Impl->Config.MaxMemoryBytes)
			{
				lua_gc(L, LUA_GCCOLLECT, 0);
				Result.Status = EAkUGCSandboxStatus::MemoryLimitExceeded;
				Result.ErrorMessage = TEXT("memory limit exceeded");
				return Result;
			}
		}
		Result.Status = EAkUGCSandboxStatus::Success;
		return Result;
	}

	// 错误归类。
	if (Impl->bInstructionLimitExceeded)
	{
		Result.Status = EAkUGCSandboxStatus::InstructionLimitExceeded;
	}
	else if (Impl->bTimeout)
	{
		Result.Status = EAkUGCSandboxStatus::Timeout;
	}
	else if (Impl->bMemoryLimitExceeded || RunStatus == LUA_ERRMEM)
	{
		Result.Status = EAkUGCSandboxStatus::MemoryLimitExceeded;
	}
	else
	{
		Result.Status = EAkUGCSandboxStatus::RuntimeError;
	}
	Result.ErrorMessage = PopTopString(L);
	return Result;
}

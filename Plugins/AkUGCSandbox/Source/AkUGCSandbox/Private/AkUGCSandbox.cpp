#include "AkUGCSandbox.h"

#include "Containers/StringConv.h"
#include "HAL/PlatformTime.h"

#include "LuaState.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

using namespace slua;

/** 沙箱 VM 内部状态。 */
struct FAkUGCSandboxImpl
{
	FAkUGCSandboxConfig Config;

	// 复用 slua 的 LuaState 创建/持有 Lua VM，不再自行 lua_newstate。
	TUniquePtr<NS_SLUA::LuaState> LuaStateOwner;
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
	const char* SavedGlobalsRegistryKey = "AkUGCSandboxSavedGlobals";

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

	/** 移除可访问文件系统/加载代码/宿主桥接的危险全局入口，并保存原始值以便关闭前恢复。 */
	void RemoveDangerousGlobals(lua_State* L)
	{
		static const char* DangerousGlobals[] = {
			// 标准库危险入口。
			"io", "os", "debug", "package", "require",
			"dofile", "loadfile", "load", "loadstring", "coroutine",
			// slua 注入的宿主桥接入口。
			"import", "slua", "slua_profile", "getStringFromMD5", "pb",
		};

		// 原始值存入注册表，供 Shutdown 恢复（slua 内部清理依赖 os 等）。
		lua_newtable(L);
		const int SavedTable = lua_gettop(L);
		for (const char* Name : DangerousGlobals)
		{
			lua_getglobal(L, Name);
			lua_setfield(L, SavedTable, Name);

			lua_pushnil(L);
			lua_setglobal(L, Name);
		}
		lua_setfield(L, LUA_REGISTRYINDEX, SavedGlobalsRegistryKey);
	}

	/** 关闭前恢复被移除的危险全局，确保 slua 内部清理（如 LuaProfiler::clean）正常执行。 */
	void RestoreDangerousGlobals(lua_State* L)
	{
		lua_getfield(L, LUA_REGISTRYINDEX, SavedGlobalsRegistryKey);
		if (!lua_istable(L, -1))
		{
			lua_pop(L, 1);
			return;
		}
		lua_pushnil(L);
		while (lua_next(L, -2) != 0)
		{
			const char* Name = lua_tostring(L, -2);
			lua_pushvalue(L, -1);
			lua_setglobal(L, Name);
			lua_pop(L, 1);
		}
		lua_pop(L, 1);
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

	// 使用 slua 创建 Lua 虚拟机。
	Impl->LuaStateOwner = MakeUnique<NS_SLUA::LuaState>("AkUGCSandbox");
	if (!Impl->LuaStateOwner->init())
	{
		Impl->LuaStateOwner.Reset();
		if (OutError)
		{
			*OutError = TEXT("failed to create Lua state via slua");
		}
		return false;
	}
	Impl->State = Impl->LuaStateOwner->getLuaState();

	// slua 已打开全库并注入宿主桥接，此处清空危险入口，仅保留安全子集。
	RemoveDangerousGlobals(Impl->State);

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
	if (Impl->State)
	{
		// 先恢复被移除的全局（如 os），确保 slua 内部清理正常执行。
		RestoreDangerousGlobals(Impl->State);
	}

	// 释放 LuaState 会触发其析构 -> close() -> lua_close。
	if (Impl->LuaStateOwner)
	{
		Impl->LuaStateOwner.Reset();
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

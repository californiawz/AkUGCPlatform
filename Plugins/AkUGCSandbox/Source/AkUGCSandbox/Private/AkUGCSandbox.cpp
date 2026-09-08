#include "AkUGCSandbox.h"

#include "Containers/StringConv.h"
#include "HAL/PlatformTime.h"

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

using namespace slua;

/** 沙箱 VM 内部状态。 */
struct FAkUGCSandboxImpl
{
	FAkUGCSandboxConfig Config;
	lua_State* State = nullptr;

	int64 InstructionsExecuted = 0;
	int64 CurrentMemoryBytes = 0;
	bool bInstructionLimitExceeded = false;
	bool bTimeout = false;
	double RunStartTime = 0.0;
};

namespace
{
	const char* SandboxImplRegistryKey = "AkUGCSandboxImpl";

	/** 内存分配器：跟踪用量，超限返回 nullptr 触发 Lua 内存错误。 */
	void* SandboxAlloc(void* UserData, void* Ptr, size_t OldSize, size_t NewSize)
	{
		FAkUGCSandboxImpl* Impl = static_cast<FAkUGCSandboxImpl*>(UserData);
		if (!Impl)
		{
			return nullptr;
		}

		if (NewSize == 0)
		{
			FMemory::Free(Ptr);
			Impl->CurrentMemoryBytes -= static_cast<int64>(OldSize);
			return nullptr;
		}

		const int64 Delta = static_cast<int64>(NewSize) - static_cast<int64>(OldSize);
		if (Impl->Config.MaxMemoryBytes > 0 && Impl->CurrentMemoryBytes + Delta > Impl->Config.MaxMemoryBytes)
		{
			return nullptr;
		}

		void* NewPtr = FMemory::Realloc(Ptr, static_cast<SIZE_T>(NewSize));
		if (NewPtr)
		{
			Impl->CurrentMemoryBytes += Delta;
		}
		return NewPtr;
	}

	/** 指令/超时 hook：累计指令并检查配额，超限即终止。 */
	void SandboxCountHook(lua_State* L, lua_Debug* /*Ar*/)
	{
		lua_getfield(L, LUA_REGISTRYINDEX, SandboxImplRegistryKey);
		FAkUGCSandboxImpl* Impl = static_cast<FAkUGCSandboxImpl*>(lua_touserdata(L, -1));
		lua_pop(L, 1);
		if (!Impl)
		{
			return;
		}

		Impl->InstructionsExecuted += Impl->Config.InstructionCheckInterval;

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
	}

	/** 仅打开安全标准库子集。 */
	void OpenSafeLibraries(lua_State* L)
	{
		luaL_requiref(L, "_G", luaopen_base, 1);
		lua_pop(L, 1);

		luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
		lua_pop(L, 1);

		luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
		lua_pop(L, 1);

		luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
		lua_pop(L, 1);

		luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
		lua_pop(L, 1);

#if defined(LUA_COMPAT_BITLIB)
		luaL_requiref(L, LUA_BITLIBNAME, luaopen_bit32, 1);
		lua_pop(L, 1);
#endif
	}

	/** 移除可访问文件系统或加载代码的危险全局函数。 */
	void RemoveDangerousGlobals(lua_State* L)
	{
		static const char* DangerousGlobals[] = { "dofile", "loadfile", "load", "loadstring" };
		for (const char* Name : DangerousGlobals)
		{
			lua_pushnil(L);
			lua_setglobal(L, Name);
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
	Impl->CurrentMemoryBytes = 0;
	Impl->InstructionsExecuted = 0;
	Impl->bInstructionLimitExceeded = false;
	Impl->bTimeout = false;

	Impl->State = lua_newstate(&SandboxAlloc, Impl.Get());
	if (!Impl->State)
	{
		if (OutError)
		{
			*OutError = TEXT("failed to create Lua state");
		}
		return false;
	}

	OpenSafeLibraries(Impl->State);
	RemoveDangerousGlobals(Impl->State);

	// 将内部状态指针存入注册表，供 hook 回调访问。
	lua_pushlightuserdata(Impl->State, Impl.Get());
	lua_setfield(Impl->State, LUA_REGISTRYINDEX, SandboxImplRegistryKey);

	// 开启指令/超时 hook。
	if (InConfig.MaxInstructionCount > 0 || InConfig.MaxRunTimeSeconds > 0.0)
	{
		const int32 Interval = (InConfig.InstructionCheckInterval > 0) ? InConfig.InstructionCheckInterval : 1000;
		lua_sethook(Impl->State, &SandboxCountHook, LUA_MASKCOUNT, Interval);
	}

	return true;
}

void FAkUGCSandbox::Shutdown()
{
	if (Impl->State)
	{
		lua_close(Impl->State);
		Impl->State = nullptr;
	}
	Impl->CurrentMemoryBytes = 0;
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
	Impl->bTimeout = false;
	Impl->RunStartTime = FPlatformTime::Seconds();

	const int RunStatus = lua_pcall(L, 0, 0, 0);
	if (RunStatus == LUA_OK)
	{
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
	else if (RunStatus == LUA_ERRMEM)
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

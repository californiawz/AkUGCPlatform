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

	// 受控 API 宿主（可选）；未注入时 ugc 命名空间不可用。
	TSharedPtr<IAkUGCSandboxHost> Host;

	int64 InstructionsExecuted = 0;
	int64 BaseMemoryBytes = 0;
	int32 HookInterval = 1000;
	bool bInstructionLimitExceeded = false;
	bool bMemoryLimitExceeded = false;
	bool bTimeout = false;
	bool bCallDepthExceeded = false;
	double RunStartTime = 0.0;

	// 受控定时器：脚本通过 ugc.timer_after 注册，宿主通过 AdvanceTimers 驱动。
	// 回调函数以 luaL_ref 存入注册表 timer 回调表，到期后由 AdvanceTimers 执行。
	struct FAkUGCSandboxPendingTimer
	{
		int32 Id = 0;
		double RemainingSeconds = 0.0;
		int32 CallbackRef = LUA_NOREF;
	};
	TArray<FAkUGCSandboxPendingTimer> PendingTimers;
	int32 NextTimerId = 1;
};

namespace
{
	const char* SandboxImplRegistryKey = "AkUGCSandboxImpl";
	const char* SandboxEnvRegistryKey = "AkUGCSandboxEnv";
	const char* SandboxTimerCallbacksKey = "AkUGCSandboxTimerCallbacks";

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

		if (Impl->Config.MaxCallDepth > 0)
		{
			int32 Depth = 0;
			lua_Debug Ar;
			while (Depth <= Impl->Config.MaxCallDepth && lua_getstack(L, Depth, &Ar))
			{
				++Depth;
			}
			if (Depth > Impl->Config.MaxCallDepth)
			{
				Impl->bCallDepthExceeded = true;
				luaL_error(L, "call depth limit exceeded");
			}
		}
	}

	/**
	 * ugc.message(msg)：把消息转发给受控 API 宿主。
	 * 宿主未注入时（注册表内无有效 Impl 或 Host 为空）抛错。
	 */
	int LuaUgcMessage(lua_State* L)
	{
		const char* Msg = luaL_checkstring(L, 1);

		lua_getfield(L, LUA_REGISTRYINDEX, SandboxImplRegistryKey);
		FAkUGCSandboxImpl* Impl = static_cast<FAkUGCSandboxImpl*>(lua_touserdata(L, -1));
		lua_pop(L, 1);

		if (!Impl || !Impl->Host)
		{
			return luaL_error(L, "ugc.message is unavailable without a host");
		}

		Impl->Host->EmitMessage(UTF8_TO_TCHAR(Msg));
		return 0;
	}

	/**
	 * ugc.get_health(entityId)：查询实体血量，返回 current, maximum。
	 * 实体不存在或无血量状态时返回 nil（不抛错，便于脚本判空）。
	 */
	int LuaUgcGetHealth(lua_State* L)
	{
		const FString EntityId = UTF8_TO_TCHAR(luaL_checkstring(L, 1));

		lua_getfield(L, LUA_REGISTRYINDEX, SandboxImplRegistryKey);
		FAkUGCSandboxImpl* Impl = static_cast<FAkUGCSandboxImpl*>(lua_touserdata(L, -1));
		lua_pop(L, 1);

		if (!Impl || !Impl->Host)
		{
			return luaL_error(L, "ugc.get_health is unavailable without a host");
		}

		double Current = 0.0;
		double Maximum = 0.0;
		if (!Impl->Host->QueryEntityHealth(EntityId, Current, Maximum))
		{
			lua_pushnil(L);
			return 1;
		}

		lua_pushnumber(L, Current);
		lua_pushnumber(L, Maximum);
		return 2;
	}

	/**
	 * ugc.apply_damage(sourceId, targetId, damage)：对目标实体造成伤害，
	 * 返回 applied, healthAfter；失败时抛错（错误消息来自宿主）。
	 */
	int LuaUgcApplyDamage(lua_State* L)
	{
		const FString SourceId = UTF8_TO_TCHAR(luaL_checkstring(L, 1));
		const FString TargetId = UTF8_TO_TCHAR(luaL_checkstring(L, 2));
		const double Damage = static_cast<double>(luaL_checknumber(L, 3));

		lua_getfield(L, LUA_REGISTRYINDEX, SandboxImplRegistryKey);
		FAkUGCSandboxImpl* Impl = static_cast<FAkUGCSandboxImpl*>(lua_touserdata(L, -1));
		lua_pop(L, 1);

		if (!Impl || !Impl->Host)
		{
			return luaL_error(L, "ugc.apply_damage is unavailable without a host");
		}

		double Applied = 0.0;
		double HealthAfter = 0.0;
		bool bKilled = false;
		FString Error;
		if (!Impl->Host->ApplyDamage(SourceId, TargetId, Damage, Applied, HealthAfter, bKilled, Error))
		{
			return luaL_error(L, TCHAR_TO_UTF8(*Error));
		}

		lua_pushnumber(L, Applied);
		lua_pushnumber(L, HealthAfter);
		return 2;
		}

		/**
		* ugc.spawn(prefabId [, anchorId])：受控生成实体，返回新实体 ID 字符串。
		* 宿主未注入或生成失败时抛错（错误消息来自宿主）。
		*/
		int LuaUgcSpawn(lua_State* L)
		{
		const FString PrefabId = UTF8_TO_TCHAR(luaL_checkstring(L, 1));
		const FString AnchorId = (lua_gettop(L) >= 2 && !lua_isnil(L, 2))
			? UTF8_TO_TCHAR(luaL_checkstring(L, 2))
			: FString();

		lua_getfield(L, LUA_REGISTRYINDEX, SandboxImplRegistryKey);
		FAkUGCSandboxImpl* Impl = static_cast<FAkUGCSandboxImpl*>(lua_touserdata(L, -1));
		lua_pop(L, 1);

		if (!Impl || !Impl->Host)
		{
			return luaL_error(L, "ugc.spawn is unavailable without a host");
		}

		FString EntityId;
		FString Error;
		if (!Impl->Host->SpawnEntity(PrefabId, AnchorId, EntityId, Error))
		{
			return luaL_error(L, TCHAR_TO_UTF8(*Error));
		}

		lua_pushstring(L, TCHAR_TO_UTF8(*EntityId));
		return 1;
		}

		/**
		* ugc.get_wave_state()：受控规则集查询，返回波次/胜负快照表。
		* 无规则集状态时返回 nil（便于脚本判空）；宿主未注入时抛错。
		*/
		int LuaUgcGetWaveState(lua_State* L)
		{
		lua_getfield(L, LUA_REGISTRYINDEX, SandboxImplRegistryKey);
		FAkUGCSandboxImpl* Impl = static_cast<FAkUGCSandboxImpl*>(lua_touserdata(L, -1));
		lua_pop(L, 1);

		if (!Impl || !Impl->Host)
		{
			return luaL_error(L, "ugc.get_wave_state is unavailable without a host");
		}

		FAkUGCSandboxWaveState State;
		if (!Impl->Host->QueryWaveState(State))
		{
			lua_pushnil(L);
			return 1;
		}

		lua_createtable(L, 0, 5);
		lua_pushinteger(L, State.WaveIndex);
		lua_setfield(L, -2, "wave_index");
		lua_pushinteger(L, State.TotalWaves);
		lua_setfield(L, -2, "total_waves");
		lua_pushinteger(L, State.State);
		lua_setfield(L, -2, "state");
		lua_pushinteger(L, State.Result);
		lua_setfield(L, -2, "result");
		lua_pushnumber(L, State.SecondsUntilNextBoundary);
		lua_setfield(L, -2, "seconds_until_boundary");
		return 1;
		}

		/**
		* ugc.timer_after(delay, callback)：注册受控延迟回调，返回 timer id。
		* delay 必须为正数；callback 存入注册表，由宿主通过 AdvanceTimers 驱动触发。
		*/
		int LuaUgcTimerAfter(lua_State* L)
		{
		const double Delay = static_cast<double>(luaL_checknumber(L, 1));
		luaL_checktype(L, 2, LUA_TFUNCTION);

		if (!(Delay > 0.0))
		{
			return luaL_error(L, "delay must be positive");
		}

		lua_getfield(L, LUA_REGISTRYINDEX, SandboxImplRegistryKey);
		FAkUGCSandboxImpl* Impl = static_cast<FAkUGCSandboxImpl*>(lua_touserdata(L, -1));
		lua_pop(L, 1);

		if (!Impl)
		{
			return luaL_error(L, "ugc.timer_after is unavailable without a sandbox");
		}

		// 将回调函数存入注册表 timer 回调表，取得整数引用。
		lua_getfield(L, LUA_REGISTRYINDEX, SandboxTimerCallbacksKey);
		lua_pushvalue(L, 2);
		const int CallbackRef = luaL_ref(L, -2);
		lua_pop(L, 1);

		FAkUGCSandboxImpl::FAkUGCSandboxPendingTimer Timer;
		Timer.Id = Impl->NextTimerId++;
		Timer.RemainingSeconds = Delay;
		Timer.CallbackRef = CallbackRef;
		Impl->PendingTimers.Add(Timer);

		lua_pushinteger(L, Timer.Id);
		return 1;
		}

		/**
		* 创建沙盒独立环境表并存入注册表。
	 *
	 * 沙盒隔离不侵入宿主 _G，而是为脚本额外创建一个独立 env：
	 *   - sandbox_env 为空表，作为脚本的 _ENV，脚本全局读写均落在该表，实例间隔离；
	 *   - sandbox_env 的 __index 指向安全库白名单，脚本仅能访问白名单内的安全库；
	 *   - 注入 Host 时，额外注册 `ugc` 命名空间承载受控 API；
	 *   - 宿主 _G 保持完整，AkLuaRuntime/slua 内部机制不受影响。
	 */
	void CreateSandboxEnv(lua_State* L, IAkUGCSandboxHost* Host)
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

		// 2) 受控 API 命名空间 ugc（仅当宿主注入时可用）。
		if (Host)
		{
			lua_newtable(L);
			lua_pushcfunction(L, &LuaUgcMessage);
			lua_setfield(L, -2, "message");
			lua_pushcfunction(L, &LuaUgcGetHealth);
			lua_setfield(L, -2, "get_health");
			lua_pushcfunction(L, &LuaUgcApplyDamage);
			lua_setfield(L, -2, "apply_damage");
			lua_pushcfunction(L, &LuaUgcSpawn);
			lua_setfield(L, -2, "spawn");
			lua_pushcfunction(L, &LuaUgcGetWaveState);
			lua_setfield(L, -2, "get_wave_state");
			lua_pushcfunction(L, &LuaUgcTimerAfter);
			lua_setfield(L, -2, "timer_after");
			lua_setfield(L, SafeGlobals, "ugc");
		}

		// 3) 沙盒 env 表（脚本的 _ENV）。
		lua_newtable(L);
		const int Env = lua_gettop(L);

		// 4) env 元表：__index = 安全库表。
		lua_newtable(L);
		lua_pushvalue(L, SafeGlobals);
		lua_setfield(L, -2, "__index");
		lua_setmetatable(L, Env);

		// 清理 safe_globals（已通过 env 元表被引用），仅留 env 在栈顶。
		lua_remove(L, SafeGlobals);

		// 存入注册表供 RunScript 使用。
		lua_setfield(L, LUA_REGISTRYINDEX, SandboxEnvRegistryKey);

		// 受控定时器回调表：ugc.timer_after 把回调引用存到这里，供 AdvanceTimers 取用。
		lua_newtable(L);
		lua_setfield(L, LUA_REGISTRYINDEX, SandboxTimerCallbacksKey);
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

bool FAkUGCSandbox::Initialize(
	const FAkUGCSandboxConfig& InConfig,
	FString* OutError,
	TSharedPtr<IAkUGCSandboxHost> InHost)
{
	Shutdown();

	Impl->Config = InConfig;
	Impl->Host = InHost;
	Impl->InstructionsExecuted = 0;
	Impl->BaseMemoryBytes = 0;
	Impl->bInstructionLimitExceeded = false;
	Impl->bMemoryLimitExceeded = false;
	Impl->bTimeout = false;
	Impl->bCallDepthExceeded = false;

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

	// 创建独立沙盒 env（不侵入宿主 _G），并注入受控 API 宿主（若提供）。
	CreateSandboxEnv(Impl->State, InHost.Get());

	// 记录基线内存，作为脚本增量配额基准（slua 初始化本身占用较大）。
	Impl->BaseMemoryBytes = GetLuaMemoryBytes(Impl->State);

	// 将内部状态指针存入注册表，供 hook 回调访问。
	lua_pushlightuserdata(Impl->State, Impl.Get());
	lua_setfield(Impl->State, LUA_REGISTRYINDEX, SandboxImplRegistryKey);

	// 开启指令/超时/调用深度 hook（任一相关配额启用即开启）。
	const bool bNeedInstructionHook =
		(InConfig.MaxInstructionCount > 0) ||
		(InConfig.MaxRunTimeSeconds > 0.0) ||
		(InConfig.MaxCallDepth > 0);
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
	Impl->Host.Reset();
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
	Impl->bCallDepthExceeded = false;
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
	else if (Impl->bCallDepthExceeded)
	{
		Result.Status = EAkUGCSandboxStatus::CallDepthExceeded;
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

int32 FAkUGCSandbox::AdvanceTimers(double DeltaSeconds)
{
	if (!Impl->State)
	{
		return 0;
	}

	lua_State* L = Impl->State;
	FLuaCallScope Scope(L);

	int32 FiredCount = 0;
	for (int32 Index = Impl->PendingTimers.Num() - 1; Index >= 0; --Index)
	{
		FAkUGCSandboxImpl::FAkUGCSandboxPendingTimer& Timer = Impl->PendingTimers[Index];
		Timer.RemainingSeconds -= DeltaSeconds;
		if (Timer.RemainingSeconds > 0.0)
		{
			continue;
		}

		// 取出回调函数（绝对索引记录 callbacks 表位置，避免栈操作漂移）。
		lua_getfield(L, LUA_REGISTRYINDEX, SandboxTimerCallbacksKey);
		const int CallbacksTable = lua_gettop(L);
		lua_rawgeti(L, CallbacksTable, Timer.CallbackRef);

		// 重置单次回调执行的配额状态（复用 RunScript 的 hook 语义）。
		Impl->InstructionsExecuted = 0;
		Impl->bInstructionLimitExceeded = false;
		Impl->bMemoryLimitExceeded = false;
		Impl->bTimeout = false;
		Impl->bCallDepthExceeded = false;
		Impl->RunStartTime = FPlatformTime::Seconds();

		const int RunStatus = lua_pcall(L, 0, 0, 0);
		++FiredCount;
		if (RunStatus != LUA_OK)
		{
			lua_pop(L, 1); // 丢弃错误信息，单个回调失败不阻断其余定时器。
		}

		// 释放回调引用并移除到期的 timer。
		luaL_unref(L, CallbacksTable, Timer.CallbackRef);
		lua_pop(L, 1); // 弹出 callbacks 表。
		Impl->PendingTimers.RemoveAt(Index);
	}

	return FiredCount;
}

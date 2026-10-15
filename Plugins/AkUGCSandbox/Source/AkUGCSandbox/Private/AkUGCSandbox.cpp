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
	bool bEffectLimitExceeded = false;
	int32 EffectCount = 0;
	int32 MessageCount = 0;
	int32 DamageCount = 0;
	int32 SpawnCount = 0;
	int32 TimerRegistrationCount = 0;
	int32 HostQueryCount = 0;
	double RunStartTime = 0.0;

	// 受控定时器：脚本通过 ugc.timer_after 注册，宿主通过 AdvanceTimers 驱动。
	// 回调函数以 luaL_ref 存入注册表 timer 回调表，到期后由 AdvanceTimers 执行。
	struct FAkUGCSandboxPendingTimer
	{
		int64 Id = 0;
		double RemainingSeconds = 0.0;
		int32 CallbackRef = LUA_NOREF;
	};
	TArray<FAkUGCSandboxPendingTimer> PendingTimers;
	int64 NextTimerId = 1;
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

	enum class EAkUGCSandboxEffectType : uint8
	{
		Message,
		Damage,
		Spawn,
		Timer
	};

	/** 重置单次 RunScript 或单个 timer callback 的执行配额。 */
	void ResetExecutionCounters(FAkUGCSandboxImpl& Impl)
	{
		Impl.InstructionsExecuted = 0;
		Impl.bInstructionLimitExceeded = false;
		Impl.bMemoryLimitExceeded = false;
		Impl.bTimeout = false;
		Impl.bCallDepthExceeded = false;
		Impl.bEffectLimitExceeded = false;
		Impl.EffectCount = 0;
		Impl.MessageCount = 0;
		Impl.DamageCount = 0;
		Impl.SpawnCount = 0;
		Impl.TimerRegistrationCount = 0;
		Impl.HostQueryCount = 0;
		Impl.RunStartTime = FPlatformTime::Seconds();
	}

	/** 严格校验规范 UTF-8：拒绝截断、孤立续字节、过长编码、代理项和越界码点。 */
	bool IsValidUtf8(const uint8* Data, const size_t Length)
	{
		size_t Index = 0;
		while (Index < Length)
		{
			const uint8 First = Data[Index++];
			if (First <= 0x7f)
			{
				continue;
			}

			int32 ContinuationCount = 0;
			uint8 SecondMin = 0x80;
			uint8 SecondMax = 0xbf;
			if (First >= 0xc2 && First <= 0xdf)
			{
				ContinuationCount = 1;
			}
			else if (First >= 0xe0 && First <= 0xef)
			{
				ContinuationCount = 2;
				if (First == 0xe0)
				{
					SecondMin = 0xa0;
				}
				else if (First == 0xed)
				{
					SecondMax = 0x9f;
				}
			}
			else if (First >= 0xf0 && First <= 0xf4)
			{
				ContinuationCount = 3;
				if (First == 0xf0)
				{
					SecondMin = 0x90;
				}
				else if (First == 0xf4)
				{
					SecondMax = 0x8f;
				}
			}
			else
			{
				return false;
			}

			if (Index + static_cast<size_t>(ContinuationCount) > Length)
			{
				return false;
			}
			const uint8 Second = Data[Index++];
			if (Second < SecondMin || Second > SecondMax)
			{
				return false;
			}
			for (int32 ContinuationIndex = 1;
				ContinuationIndex < ContinuationCount;
				++ContinuationIndex)
			{
				const uint8 Continuation = Data[Index++];
				if (Continuation < 0x80 || Continuation > 0xbf)
				{
					return false;
				}
			}
		}
		return true;
	}

	/** 位级有限性检查，不受模块浮点优化模式对 NaN/Inf 假设的影响。 */
	bool IsFiniteDouble(const double Value)
	{
		uint64 Bits = 0;
		static_assert(sizeof(Bits) == sizeof(Value));
		FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
		return (Bits & 0x7ff0000000000000ULL) != 0x7ff0000000000000ULL;
	}

	/** 精确读取并验证有界 UTF-8，避免 luaL_checkstring 的 NUL 截断与无界转换。 */
	bool ReadBoundedUtf8(
		lua_State* L,
		const int32 StackIndex,
		const int32 MaxBytes,
		const TCHAR* ArgumentName,
		FString& OutValue,
		FString& OutError)
	{
		size_t Length = 0;
		if (lua_type(L, StackIndex) != LUA_TSTRING)
		{
			OutError = FString::Printf(TEXT("%s must be a string"), ArgumentName);
			return false;
		}
		const char* Value = lua_tolstring(L, StackIndex, &Length);
		if (!Value)
		{
			OutError = FString::Printf(TEXT("%s could not be read"), ArgumentName);
			return false;
		}
		if (Length > static_cast<size_t>(MaxBytes))
		{
			OutError = FString::Printf(
				TEXT("%s exceeds UTF-8 byte limit (%llu > %d)"),
				ArgumentName,
				static_cast<uint64>(Length),
				MaxBytes);
			return false;
		}
		bool bContainsNul = false;
		for (size_t Index = 0; Index < Length; ++Index)
		{
			if (Value[Index] == '\0')
			{
				bContainsNul = true;
				break;
			}
		}
		if (bContainsNul)
		{
			OutError = FString::Printf(TEXT("%s contains an embedded NUL byte"), ArgumentName);
			return false;
		}
		if (!IsValidUtf8(reinterpret_cast<const uint8*>(Value), Length))
		{
			OutError = FString::Printf(TEXT("%s is not valid UTF-8"), ArgumentName);
			return false;
		}
		if (Length == 0)
		{
			OutValue.Reset();
			return true;
		}

		const FUTF8ToTCHAR Converted(Value, static_cast<int32>(Length));
		OutValue = FString(Converted.Length(), Converted.Get());

		// 非法序列会在转换时被替换；回转比较确保宿主只收到完整、规范的 UTF-8。
		const FTCHARToUTF8 RoundTrip(*OutValue, OutValue.Len());
		if (RoundTrip.Length() != static_cast<int32>(Length)
			|| FMemory::Memcmp(RoundTrip.Get(), Value, Length) != 0)
		{
			OutValue.Reset();
			OutError = FString::Printf(TEXT("%s is not valid UTF-8"), ArgumentName);
			return false;
		}
		return true;
	}

	/** 在调用宿主前原子消费总副作用与分类型配额。 */
	bool TryConsumeEffect(
		FAkUGCSandboxImpl& Impl,
		const EAkUGCSandboxEffectType EffectType,
		FString& OutError)
	{
		if (Impl.bEffectLimitExceeded)
		{
			OutError = TEXT("sandbox quota rejection is already latched for this execution");
			return false;
		}

		int32* TypeCount = nullptr;
		int32 TypeLimit = 0;
		const TCHAR* TypeName = nullptr;
		switch (EffectType)
		{
		case EAkUGCSandboxEffectType::Message:
			TypeCount = &Impl.MessageCount;
			TypeLimit = Impl.Config.MaxMessageCount;
			TypeName = TEXT("message");
			break;
		case EAkUGCSandboxEffectType::Damage:
			TypeCount = &Impl.DamageCount;
			TypeLimit = Impl.Config.MaxDamageCount;
			TypeName = TEXT("damage");
			break;
		case EAkUGCSandboxEffectType::Spawn:
			TypeCount = &Impl.SpawnCount;
			TypeLimit = Impl.Config.MaxSpawnCount;
			TypeName = TEXT("spawn");
			break;
		case EAkUGCSandboxEffectType::Timer:
			TypeCount = &Impl.TimerRegistrationCount;
			TypeLimit = Impl.Config.MaxTimerRegistrationCount;
			TypeName = TEXT("timer registration");
			break;
		default:
			checkNoEntry();
			return false;
		}

		if (Impl.EffectCount >= Impl.Config.MaxEffectCount)
		{
			Impl.bEffectLimitExceeded = true;
			OutError = FString::Printf(
				TEXT("sandbox total effect quota exceeded (%d)"),
				Impl.Config.MaxEffectCount);
			return false;
		}
		if (*TypeCount >= TypeLimit)
		{
			Impl.bEffectLimitExceeded = true;
			OutError = FString::Printf(
				TEXT("sandbox %s quota exceeded (%d)"),
				TypeName,
				TypeLimit);
			return false;
		}

		++Impl.EffectCount;
		++(*TypeCount);
		return true;
	}

	/** 在调用宿主前消费只读查询配额。 */
	bool TryConsumeHostQuery(FAkUGCSandboxImpl& Impl, FString& OutError)
	{
		if (Impl.bEffectLimitExceeded)
		{
			OutError = TEXT("sandbox quota rejection is already latched for this execution");
			return false;
		}
		if (Impl.HostQueryCount >= Impl.Config.MaxHostQueryCount)
		{
			Impl.bEffectLimitExceeded = true;
			OutError = FString::Printf(
				TEXT("sandbox host query quota exceeded (%d)"),
				Impl.Config.MaxHostQueryCount);
			return false;
		}
		++Impl.HostQueryCount;
		return true;
	}

	/**
	 * 原始 C API 不跨 native 回调边界抛 Lua 错误，而是返回 ok, values...。
	 * CreateSandboxEnv 安装的 Lua 包装器会在 C 回调正常返回后把失败转换为 Lua error。
	 */
	int PushControlledApiFailure(lua_State* L, const FString& Error)
	{
		const FString SafeError = Error.IsEmpty()
			? FString(TEXT("controlled API request failed"))
			: Error;
		const FTCHARToUTF8 ErrorUtf8(*SafeError, SafeError.Len());
		lua_pushboolean(L, 0);
		lua_pushlstring(L, ErrorUtf8.Get(), ErrorUtf8.Length());
		return 2;
	}

	int PushControlledApiFailure(lua_State* L, const ANSICHAR* Error)
	{
		lua_pushboolean(L, 0);
		lua_pushstring(L, Error);
		return 2;
	}

	/**
	 * ugc.message(msg)：把消息转发给受控 API 宿主。
	 * 宿主未注入时（注册表内无有效 Impl 或 Host 为空）抛错。
	 */
	int LuaUgcMessage(lua_State* L)
	{
		FString Message;
		FString Error;
		if (!ReadBoundedUtf8(
			L,
			1,
			FAkUGCSandbox::MaxMessageBytes,
			TEXT("message"),
			Message,
			Error))
		{
			return PushControlledApiFailure(L, Error);
		}

		lua_getfield(L, LUA_REGISTRYINDEX, SandboxImplRegistryKey);
		FAkUGCSandboxImpl* Impl = static_cast<FAkUGCSandboxImpl*>(lua_touserdata(L, -1));
		lua_pop(L, 1);

		if (!Impl || !Impl->Host)
		{
			return PushControlledApiFailure(L, "ugc.message is unavailable without a host");
		}

		if (!TryConsumeEffect(*Impl, EAkUGCSandboxEffectType::Message, Error))
		{
			return PushControlledApiFailure(L, Error);
		}
		Impl->Host->EmitMessage(Message);
		lua_pushboolean(L, 1);
		return 1;
	}

	/**
	 * ugc.get_health(entityId)：查询实体血量，返回 current, maximum。
	 * 实体不存在或无血量状态时返回 nil（不抛错，便于脚本判空）。
	 */
	int LuaUgcGetHealth(lua_State* L)
	{
		FString EntityId;
		FString Error;
		if (!ReadBoundedUtf8(
			L,
			1,
			FAkUGCSandbox::MaxIdentifierBytes,
			TEXT("entity id"),
			EntityId,
			Error))
		{
			return PushControlledApiFailure(L, Error);
		}
		if (EntityId.IsEmpty())
		{
			return PushControlledApiFailure(L, "entity id must not be empty");
		}

		lua_getfield(L, LUA_REGISTRYINDEX, SandboxImplRegistryKey);
		FAkUGCSandboxImpl* Impl = static_cast<FAkUGCSandboxImpl*>(lua_touserdata(L, -1));
		lua_pop(L, 1);

		if (!Impl || !Impl->Host)
		{
			return PushControlledApiFailure(L, "ugc.get_health is unavailable without a host");
		}
		if (!TryConsumeHostQuery(*Impl, Error))
		{
			return PushControlledApiFailure(L, Error);
		}

		double Current = 0.0;
		double Maximum = 0.0;
		if (!Impl->Host->QueryEntityHealth(EntityId, Current, Maximum))
		{
			lua_pushboolean(L, 1);
			lua_pushnil(L);
			return 2;
		}

		lua_pushboolean(L, 1);
		lua_pushnumber(L, Current);
		lua_pushnumber(L, Maximum);
		return 3;
	}

	/**
	 * ugc.apply_damage(sourceId, targetId, damage)：对目标实体造成伤害，
	 * 返回 applied, healthAfter；失败时抛错（错误消息来自宿主）。
	 */
	int LuaUgcApplyDamage(lua_State* L)
	{
		if (lua_type(L, 3) != LUA_TNUMBER)
		{
			return PushControlledApiFailure(L, "damage must be a number");
		}
		const double Damage = static_cast<double>(lua_tonumber(L, 3));
		if (!IsFiniteDouble(Damage)
			|| !(Damage > 0.0)
			|| Damage > FAkUGCSandbox::MaxDamagePerCall)
		{
			return PushControlledApiFailure(
				L,
				"damage must be finite and in the range (0, 1000000]");
		}

		FString SourceId;
		FString TargetId;
		FString Error;
		if (!ReadBoundedUtf8(
			L,
			1,
			FAkUGCSandbox::MaxIdentifierBytes,
			TEXT("source id"),
			SourceId,
			Error)
			|| !ReadBoundedUtf8(
				L,
				2,
				FAkUGCSandbox::MaxIdentifierBytes,
				TEXT("target id"),
				TargetId,
				Error))
		{
			return PushControlledApiFailure(L, Error);
		}
		if (SourceId.IsEmpty() || TargetId.IsEmpty())
		{
			return PushControlledApiFailure(L, "source id and target id must not be empty");
		}

		lua_getfield(L, LUA_REGISTRYINDEX, SandboxImplRegistryKey);
		FAkUGCSandboxImpl* Impl = static_cast<FAkUGCSandboxImpl*>(lua_touserdata(L, -1));
		lua_pop(L, 1);

		if (!Impl || !Impl->Host)
		{
			return PushControlledApiFailure(L, "ugc.apply_damage is unavailable without a host");
		}
		if (!TryConsumeEffect(*Impl, EAkUGCSandboxEffectType::Damage, Error))
		{
			return PushControlledApiFailure(L, Error);
		}

		double Applied = 0.0;
		double HealthAfter = 0.0;
		bool bKilled = false;
		if (!Impl->Host->ApplyDamage(SourceId, TargetId, Damage, Applied, HealthAfter, bKilled, Error))
		{
			return PushControlledApiFailure(L, Error);
		}

		lua_pushboolean(L, 1);
		lua_pushnumber(L, Applied);
		lua_pushnumber(L, HealthAfter);
		return 3;
		}

		/**
		* ugc.spawn(prefabId [, anchorId])：受控生成实体，返回新实体 ID 字符串。
		* 宿主未注入或生成失败时抛错（错误消息来自宿主）。
		*/
		int LuaUgcSpawn(lua_State* L)
		{
		FString PrefabId;
		FString AnchorId;
		FString Error;
		if (!ReadBoundedUtf8(
			L,
			1,
			FAkUGCSandbox::MaxIdentifierBytes,
			TEXT("prefab id"),
			PrefabId,
			Error))
		{
			return PushControlledApiFailure(L, Error);
		}
		if (PrefabId.IsEmpty())
		{
			return PushControlledApiFailure(L, "prefab id must not be empty");
		}
		if (lua_gettop(L) >= 2
			&& !lua_isnil(L, 2)
			&& !ReadBoundedUtf8(
				L,
				2,
				FAkUGCSandbox::MaxIdentifierBytes,
				TEXT("anchor id"),
				AnchorId,
				Error))
		{
			return PushControlledApiFailure(L, Error);
		}
		if (lua_gettop(L) >= 2 && !lua_isnil(L, 2) && AnchorId.IsEmpty())
		{
			return PushControlledApiFailure(L, "anchor id must not be empty when provided");
		}

		lua_getfield(L, LUA_REGISTRYINDEX, SandboxImplRegistryKey);
		FAkUGCSandboxImpl* Impl = static_cast<FAkUGCSandboxImpl*>(lua_touserdata(L, -1));
		lua_pop(L, 1);

		if (!Impl || !Impl->Host)
		{
			return PushControlledApiFailure(L, "ugc.spawn is unavailable without a host");
		}
		if (!TryConsumeEffect(*Impl, EAkUGCSandboxEffectType::Spawn, Error))
		{
			return PushControlledApiFailure(L, Error);
		}

		FString EntityId;
		if (!Impl->Host->SpawnEntity(PrefabId, AnchorId, EntityId, Error))
		{
			return PushControlledApiFailure(L, Error);
		}

		const FTCHARToUTF8 EntityIdUtf8(*EntityId, EntityId.Len());
		if (EntityIdUtf8.Length() > FAkUGCSandbox::MaxIdentifierBytes)
		{
			return PushControlledApiFailure(
				L,
				"host returned an entity id above the UTF-8 byte limit");
		}
		lua_pushboolean(L, 1);
		lua_pushlstring(L, EntityIdUtf8.Get(), EntityIdUtf8.Length());
		return 2;
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
			return PushControlledApiFailure(L, "ugc.get_wave_state is unavailable without a host");
		}
		FString Error;
		if (!TryConsumeHostQuery(*Impl, Error))
		{
			return PushControlledApiFailure(L, Error);
		}

		FAkUGCSandboxWaveState State;
		if (!Impl->Host->QueryWaveState(State))
		{
			lua_pushboolean(L, 1);
			lua_pushnil(L);
			return 2;
		}

		lua_pushboolean(L, 1);
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
		return 2;
		}

		/**
		* ugc.timer_after(delay, callback)：注册受控延迟回调，返回 timer id。
		* delay 必须为有限正数且不超过 MaxTimerDelaySeconds；
		* callback 存入注册表，由宿主通过 AdvanceTimers 驱动触发。
		*/
		int LuaUgcTimerAfter(lua_State* L)
		{
		if (lua_type(L, 1) != LUA_TNUMBER)
		{
			return PushControlledApiFailure(L, "delay must be a number");
		}
		if (lua_type(L, 2) != LUA_TFUNCTION)
		{
			return PushControlledApiFailure(L, "timer callback must be a function");
		}
		const double Delay = static_cast<double>(lua_tonumber(L, 1));

		if (!IsFiniteDouble(Delay)
			|| !(Delay > 0.0)
			|| Delay > FAkUGCSandbox::MaxTimerDelaySeconds)
		{
			return PushControlledApiFailure(
				L,
				"delay must be finite and in the range (0, 3600]");
		}

		lua_getfield(L, LUA_REGISTRYINDEX, SandboxImplRegistryKey);
		FAkUGCSandboxImpl* Impl = static_cast<FAkUGCSandboxImpl*>(lua_touserdata(L, -1));
		lua_pop(L, 1);

		if (!Impl)
		{
			return PushControlledApiFailure(L, "ugc.timer_after is unavailable without a sandbox");
		}
		FString Error;
		if (!TryConsumeEffect(*Impl, EAkUGCSandboxEffectType::Timer, Error))
		{
			return PushControlledApiFailure(L, Error);
		}
		if (Impl->PendingTimers.Num() >= Impl->Config.MaxPendingTimerCount)
		{
			Impl->bEffectLimitExceeded = true;
			return PushControlledApiFailure(
				L,
				FString::Printf(
					TEXT("sandbox pending timer quota exceeded (%d)"),
					Impl->Config.MaxPendingTimerCount));
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

		lua_pushboolean(L, 1);
		lua_pushinteger(L, Timer.Id);
		return 2;
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
	bool CreateSandboxEnv(lua_State* L, IAkUGCSandboxHost* Host, FString& OutError)
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
			static const ANSICHAR* WrapperFactorySource =
				"local pack, unpack, raise = table.pack, table.unpack, error\n"
				"return function(raw)\n"
				"  return function(...)\n"
				"    local values = pack(raw(...))\n"
				"    if not values[1] then raise(values[2], 2) end\n"
				"    return unpack(values, 2, values.n)\n"
				"  end\n"
				"end\n";
			if (luaL_loadbuffer(
				L,
				WrapperFactorySource,
				FCStringAnsi::Strlen(WrapperFactorySource),
				"=controlled_api_wrapper") != LUA_OK
				|| lua_pcall(L, 0, 1, 0) != LUA_OK)
			{
				OutError = TEXT("failed to create controlled API Lua wrappers");
				lua_pop(L, 1);
				return false;
			}
			const int WrapperFactory = lua_gettop(L);

			lua_newtable(L);
			const int UgcTable = lua_gettop(L);
			const auto RegisterControlledApi =
				[L, WrapperFactory, UgcTable](lua_CFunction RawFunction, const ANSICHAR* Name)
				{
					lua_pushvalue(L, WrapperFactory);
					lua_pushcfunction(L, RawFunction);
					lua_call(L, 1, 1);
					lua_setfield(L, UgcTable, Name);
				};
			RegisterControlledApi(&LuaUgcMessage, "message");
			RegisterControlledApi(&LuaUgcGetHealth, "get_health");
			RegisterControlledApi(&LuaUgcApplyDamage, "apply_damage");
			RegisterControlledApi(&LuaUgcSpawn, "spawn");
			RegisterControlledApi(&LuaUgcGetWaveState, "get_wave_state");
			RegisterControlledApi(&LuaUgcTimerAfter, "timer_after");

			lua_remove(L, WrapperFactory);
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
		return true;
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

	if (InConfig.MaxEffectCount <= 0
		|| InConfig.MaxMessageCount <= 0
		|| InConfig.MaxDamageCount <= 0
		|| InConfig.MaxSpawnCount <= 0
		|| InConfig.MaxTimerRegistrationCount <= 0
		|| InConfig.MaxHostQueryCount <= 0
		|| InConfig.MaxPendingTimerCount <= 0
		|| InConfig.MaxTimerCallbacksPerAdvance <= 0)
	{
		if (OutError)
		{
			*OutError = TEXT("sandbox effect, query, and timer quotas must all be positive");
		}
		return false;
	}

	Impl->Config = InConfig;
	Impl->Host = InHost;
	Impl->BaseMemoryBytes = 0;
	ResetExecutionCounters(*Impl);

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
	FString EnvironmentError;
	if (!CreateSandboxEnv(Impl->State, InHost.Get(), EnvironmentError))
	{
		Shutdown();
		if (OutError)
		{
			*OutError = MoveTemp(EnvironmentError);
		}
		return false;
	}

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
	Impl->PendingTimers.Reset();
	Impl->NextTimerId = 1;
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

	if (Source.Len() > MaxScriptSourceBytes
		|| ChunkName.Len() > MaxChunkNameBytes)
	{
		Result.Status = EAkUGCSandboxStatus::InputLimitExceeded;
		Result.ErrorMessage = TEXT("sandbox script input exceeds its UTF-8 byte budget");
		return Result;
	}
	const FTCHARToUTF8 SourceUtf8(*Source, Source.Len());
	const FTCHARToUTF8 ChunkNameUtf8(*ChunkName, ChunkName.Len());
	if (SourceUtf8.Length() > MaxScriptSourceBytes
		|| ChunkNameUtf8.Length() > MaxChunkNameBytes)
	{
		Result.Status = EAkUGCSandboxStatus::InputLimitExceeded;
		Result.ErrorMessage = TEXT("sandbox script input exceeds its UTF-8 byte budget");
		return Result;
	}

	lua_State* L = Impl->State;
	FLuaCallScope Scope(L);

	// 编译源码。
	const int LoadStatus = luaL_loadbuffer(
		L,
		SourceUtf8.Get(),
		SourceUtf8.Length(),
		ChunkNameUtf8.Get());
	if (LoadStatus != LUA_OK)
	{
		Result.Status = EAkUGCSandboxStatus::CompileError;
		Result.ErrorMessage = PopTopString(L);
		return Result;
	}

	// 将 chunk 的 _ENV 指向沙盒独立环境表。
	SetChunkEnv(L);

	// 运行。
	ResetExecutionCounters(*Impl);

	const int RunStatus = lua_pcall(L, 0, 0, 0);
	if (RunStatus == LUA_OK)
	{
		// Lua 可以用 pcall 捕获受控 API 错误，但不能把配额拒绝伪装成成功执行。
		if (Impl->bEffectLimitExceeded)
		{
			Result.Status = EAkUGCSandboxStatus::EffectLimitExceeded;
			Result.ErrorMessage = TEXT("sandbox effect, query, or timer quota exceeded");
			return Result;
		}

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
	else if (Impl->bEffectLimitExceeded)
	{
		Result.Status = EAkUGCSandboxStatus::EffectLimitExceeded;
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
	if (!Impl->State || !FMath::IsFinite(DeltaSeconds) || !(DeltaSeconds > 0.0))
	{
		return 0;
	}

	lua_State* L = Impl->State;
	FLuaCallScope Scope(L);

	// 先统一推进本 Tick 已存在的 timer；回调中新注册的 timer 从下一 Tick 开始计时。
	for (FAkUGCSandboxImpl::FAkUGCSandboxPendingTimer& Timer : Impl->PendingTimers)
	{
		Timer.RemainingSeconds -= DeltaSeconds;
	}

	int32 FiredCount = 0;
	for (int32 Index = Impl->PendingTimers.Num() - 1;
		Index >= 0 && FiredCount < Impl->Config.MaxTimerCallbacksPerAdvance;
		--Index)
	{
		const FAkUGCSandboxImpl::FAkUGCSandboxPendingTimer Timer = Impl->PendingTimers[Index];
		if (Timer.RemainingSeconds > 0.0)
		{
			continue;
		}

		// 先移除 pending 项和注册表引用，避免回调注册新 timer 时令数组引用失效。
		Impl->PendingTimers.RemoveAt(Index);
		lua_getfield(L, LUA_REGISTRYINDEX, SandboxTimerCallbacksKey);
		const int CallbacksTable = lua_gettop(L);
		lua_rawgeti(L, CallbacksTable, Timer.CallbackRef);
		luaL_unref(L, CallbacksTable, Timer.CallbackRef);
		lua_remove(L, CallbacksTable); // 仅保留待调用函数。

		// 重置单次回调执行的配额状态（复用 RunScript 的 hook 语义）。
		ResetExecutionCounters(*Impl);

		const int RunStatus = lua_pcall(L, 0, 0, 0);
		++FiredCount;
		if (RunStatus != LUA_OK)
		{
			lua_pop(L, 1); // 丢弃错误信息，单个回调失败不阻断其余定时器。
		}
	}

	return FiredCount;
}

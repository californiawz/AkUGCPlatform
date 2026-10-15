#pragma once

#include "CoreMinimal.h"
#include "IAkUGCSandboxHost.h"

/** 沙箱执行结果状态。 */
enum class EAkUGCSandboxStatus : uint8
{
	/** 成功执行。 */
	Success,
	/** 编译（语法）错误。 */
	CompileError,
	/** 运行时错误。 */
	RuntimeError,
	/** 指令配额超限。 */
	InstructionLimitExceeded,
	/** 内存配额超限。 */
	MemoryLimitExceeded,
	/** 运行超时。 */
	Timeout,
	/** 调用深度配额超限。 */
	CallDepthExceeded,
	/** 副作用、宿主查询或挂起资源配额超限。 */
	EffectLimitExceeded,
	/** 脚本源码或 chunk 名称超过解析前输入预算。 */
	InputLimitExceeded,
};

/** 沙箱配额配置。 */
struct AKUGCSANDBOX_API FAkUGCSandboxConfig
{
	/** 指令配额上限，0 表示不限制。 */
	int64 MaxInstructionCount = 1000000;

	/** 内存配额上限（字节），0 表示不限制。 */
	int64 MaxMemoryBytes = 4 * 1024 * 1024;

	/** 运行超时（秒），0 表示不限制。 */
	double MaxRunTimeSeconds = 0.0;

	/** 指令 hook 检查间隔（每 N 条指令检查一次配额与超时）。 */
	int32 InstructionCheckInterval = 1000;

	/** 最大调用深度（嵌套 Lua 调用层数），0 表示不限制。 */
	int32 MaxCallDepth = 0;

	/**
	 * 单次执行（RunScript 或一个定时器回调）允许的受控副作用尝试总数。
	 * 包括 message / apply_damage / spawn / timer_after；必须为正数。
	 */
	int32 MaxEffectCount = 64;

	/** 单次执行允许的消息副作用尝试数；必须为正数。 */
	int32 MaxMessageCount = 32;

	/** 单次执行允许的伤害副作用尝试数；必须为正数。 */
	int32 MaxDamageCount = 32;

	/** 单次执行允许的生成副作用尝试数；必须为正数。 */
	int32 MaxSpawnCount = 16;

	/** 单次执行允许注册的定时器数；必须为正数。 */
	int32 MaxTimerRegistrationCount = 16;

	/** 单次执行允许的只读宿主查询数；必须为正数。 */
	int32 MaxHostQueryCount = 128;

	/** 单个沙箱同时保留的定时器上限；跨 RunScript 生效，必须为正数。 */
	int32 MaxPendingTimerCount = 64;

	/** 一次 AdvanceTimers 最多触发的回调数；其余到期回调留待下次，必须为正数。 */
	int32 MaxTimerCallbacksPerAdvance = 16;
};

/** 沙箱执行结果。 */
struct AKUGCSANDBOX_API FAkUGCSandboxResult
{
	EAkUGCSandboxStatus Status = EAkUGCSandboxStatus::Success;
	FString ErrorMessage;
};

struct FAkUGCSandboxImpl;

/**
 * 独立 Lua 沙箱。
 *
 * VM 生命周期参考 AkLuaRuntime（复用 slua 的 LuaState，而非自行 lua_newstate）。
 * 沙盒隔离通过「为每个实例额外创建一个独立 env 表（_ENV）」实现，而非清空宿主全局：
 *   - 脚本的全局读写均落在独立 env 表，实例间互不可见；
 *   - env 的 __index 仅指向安全库白名单（base 精简 / table / string / math / utf8），
 *     无法访问 io / os / debug / package / require / dofile / loadfile / load 等；
 *   - 宿主 _G 保持完整，slua 内部机制不受影响。
 * 提供指令、内存、时间、调用深度、受控副作用、宿主查询和定时器资源配额；
 * 配额拒绝会在宿主调用前闭锁当前执行，不能被 Lua pcall 隐藏为成功。
 */
class AKUGCSANDBOX_API FAkUGCSandbox
{
public:
	/** Lua 编译前允许的最大 UTF-8 源码字节数。 */
	static constexpr int64 MaxScriptSourceBytes = 128 * 1024;

	/** Lua 编译前允许的最大 UTF-8 chunk 名称字节数。 */
	static constexpr int32 MaxChunkNameBytes = 256;

	/** 受控 API 消息参数的最大 UTF-8 字节数。 */
	static constexpr int32 MaxMessageBytes = 1024;

	/** 受控 API 标识符参数的最大 UTF-8 字节数。 */
	static constexpr int32 MaxIdentifierBytes = 128;

	/** 受控定时器允许的最大延迟。 */
	static constexpr double MaxTimerDelaySeconds = 3600.0;

	/** 单次受控伤害请求允许的最大值。 */
	static constexpr double MaxDamagePerCall = 1000000.0;

	FAkUGCSandbox();
	~FAkUGCSandbox();

	FAkUGCSandbox(const FAkUGCSandbox&) = delete;
	FAkUGCSandbox& operator=(const FAkUGCSandbox&) = delete;

	/**
	 * 初始化沙箱 VM。失败时 OutError 返回原因。
	 *
	 * InHost 可选：注入受控 API 宿主后，脚本可在 `ugc` 命名空间调用受控能力；
	 * 未注入时 `ugc` 为 nil（安全默认）。Host 以 TSharedPtr 持有强引用。
	 */
	bool Initialize(
		const FAkUGCSandboxConfig& InConfig,
		FString* OutError = nullptr,
		TSharedPtr<IAkUGCSandboxHost> InHost = nullptr);

	/** 关闭并释放 VM。 */
	void Shutdown();

	/** 是否已初始化。 */
	bool IsInitialized() const;

	/** 执行一段 Lua 脚本，返回执行结果。 */
	FAkUGCSandboxResult RunScript(const FString& Source, const FString& ChunkName = TEXT("=sandbox"));

	/**
	 * 推进受控定时器：扣减剩余时间并触发到期回调，返回本次触发的回调数量。
	 * 由宿主（如场景运行时 Tick）周期性调用；回调在沙箱 env 下执行，仍受各类配额约束，
	 * 且每次调用最多触发 MaxTimerCallbacksPerAdvance 个回调。
	 */
	int32 AdvanceTimers(double DeltaSeconds);

private:
	TUniquePtr<FAkUGCSandboxImpl> Impl;
};

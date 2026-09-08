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
 * 提供指令、内存与运行时间三类配额，超限即安全终止而不影响宿主。
 */
class AKUGCSANDBOX_API FAkUGCSandbox
{
public:
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

private:
	TUniquePtr<FAkUGCSandboxImpl> Impl;
};

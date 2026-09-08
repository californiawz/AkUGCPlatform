#pragma once

#include "CoreMinimal.h"

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
};

/** 沙箱执行结果。 */
struct AKUGCSANDBOX_API FAkUGCSandboxResult
{
	EAkUGCSandboxStatus Status = EAkUGCSandboxStatus::Success;
	FString ErrorMessage;
};

struct FAkUGCSandboxImpl;

/**
 * 独立 Lua 5.3.4 沙箱。
 *
 * 每个实例持有一个独立 VM，与其他实例完全隔离；
 * 仅开放安全标准库子集（base 精简 / table / string / math / utf8 / bit32），
 * 关闭 io / os / debug / package / require / dofile / loadfile / load；
 * 提供指令、内存与运行时间三类配额，超限即安全终止而不影响宿主。
 */
class AKUGCSANDBOX_API FAkUGCSandbox
{
public:
	FAkUGCSandbox();
	~FAkUGCSandbox();

	FAkUGCSandbox(const FAkUGCSandbox&) = delete;
	FAkUGCSandbox& operator=(const FAkUGCSandbox&) = delete;

	/** 初始化沙箱 VM。失败时 OutError 返回原因。 */
	bool Initialize(const FAkUGCSandboxConfig& InConfig, FString* OutError = nullptr);

	/** 关闭并释放 VM。 */
	void Shutdown();

	/** 是否已初始化。 */
	bool IsInitialized() const;

	/** 执行一段 Lua 脚本，返回执行结果。 */
	FAkUGCSandboxResult RunScript(const FString& Source, const FString& ChunkName = TEXT("=sandbox"));

private:
	TUniquePtr<FAkUGCSandboxImpl> Impl;
};

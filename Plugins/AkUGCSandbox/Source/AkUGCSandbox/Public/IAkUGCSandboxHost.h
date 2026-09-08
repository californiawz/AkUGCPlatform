// Copyright Akatsuki. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * IAkUGCSandboxHost
 *
 * 沙箱受控 API 的宿主接口。UGC 运行时（如权威玩法会话 / 逻辑运行时子系统）
 * 通过实现该接口，把受控能力注入沙箱，脚本得以在 `ugc` 命名空间下调用。
 *
 * 沙箱本身保持游戏无关：它只认识这个抽象接口，不直接依赖任何具体运行时。
 * 未注入 Host 时，沙箱脚本无法访问 `ugc` 命名空间（安全默认）。
 *
 * 沙箱在 Initialize 时以 TSharedPtr 持有 Host 强引用，因此 Host 既可以是
 * 独立对象，也可以由使用方以 TSharedPtr 管理，其生命周期只需满足：
 * 在沙箱 RunScript 期间有效即可。
 */
class AKUGCSANDBOX_API IAkUGCSandboxHost
{
public:
	virtual ~IAkUGCSandboxHost() = default;

	/** 受控消息：脚本调用 ugc.message(msg) 时触发。 */
	virtual void EmitMessage(const FString& Message) = 0;
};

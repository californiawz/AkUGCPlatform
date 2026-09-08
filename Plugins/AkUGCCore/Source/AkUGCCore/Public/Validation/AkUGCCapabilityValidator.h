#pragma once

#include "CoreMinimal.h"

/**
 * FAkUGCCapabilityValidator
 *
 * 能力（Capability）校验器。作品 Manifest 声明其使用的运行时能力集合，
 * 运行端（Client 与 Dedicated Server）在加载 Logic Pack 时必须重复校验：
 * 声明中任何本运行时不支持或未知的能力都会导致加载被拒绝。
 *
 * 该校验器是无状态纯函数，Client 与 Server 复用同一实现，从而保证
 * 被篡改或声明了越权能力的包在两端都会被一致拒绝。
 */
class AKUGCCORE_API FAkUGCCapabilityValidator
{
public:
	/** 本运行端支持的能力白名单（Phase 0 三波合作塔防切片）。 */
	static TArray<FName> GetSupportedCapabilities();

	/**
	 * 校验能力集合。所有声明能力都必须在白名单内；返回 false 时 OutError
	 * 说明首个未支持的能力。
	 */
	static bool Validate(const TArray<FName>& Capabilities, FString* OutError = nullptr);
};

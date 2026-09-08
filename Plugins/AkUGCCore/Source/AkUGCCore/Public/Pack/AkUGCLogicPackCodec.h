#pragma once

#include "CoreMinimal.h"
#include "Pack/AkUGCLogicPack.h"

/**
 * Logic Pack JSON 编解码器。
 *
 * 负责发布包在内存结构与 JSON 文本之间往返。
 * Document 复用 FAkUGCDocumentJson（含迁移与严格校验），
 * Manifest 与 Logic IR 使用确定性字段名手写编解码。
 */
class AKUGCCORE_API FAkUGCLogicPackCodec
{
public:
	/** 将发布包序列化为 JSON 文本。 */
	static bool Serialize(const FAkUGCLogicPack& Pack, FString& OutJson, FString* OutError = nullptr);

	/** 将 JSON 文本反序列化为发布包。 */
	static bool Deserialize(const FString& Json, FAkUGCLogicPack& OutPack, FString* OutError = nullptr);
};

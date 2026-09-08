#pragma once

#include "CoreMinimal.h"
#include "Pack/AkUGCLogicPack.h"

struct AKUGCCORE_API FAkUGCLogicPackLoadResult
{
	bool bSucceeded = false;
	FString ErrorMessage;
	FAkUGCLogicPack Pack;
};

/**
 * Logic Pack 加载器。
 *
 * 反序列化发布包并进行完整性校验：
 *  - Schema 版本兼容
 *  - 内容确定性哈希一致（拒绝被修改的包）
 *  - 文档合法
 *  - Logic IR 与文档重新编译结果一致（拒绝文档与 IR 不匹配的包）
 */
class AKUGCCORE_API FAkUGCLogicPackLoader
{
public:
	/** 加载并校验发布包 JSON。 */
	static FAkUGCLogicPackLoadResult Load(const FString& Json);
};

#pragma once

#include "CoreMinimal.h"
#include "Pack/AkUGCLogicPack.h"

struct AKUGCCORE_API FAkUGCLogicPackBuildResult
{
	bool bSucceeded = false;
	FString ErrorMessage;
	FAkUGCLogicPack Pack;
};

/**
 * Logic Pack 构建器。
 *
 * 将作品文档校验并编译为 Logic IR，生成发布清单并计算内容确定性哈希，
 * 组装为可发布、不可变的 FAkUGCLogicPack。
 */
class AKUGCCORE_API FAkUGCLogicPackBuilder
{
public:
	/** 从作品文档构建发布包。 */
	static FAkUGCLogicPackBuildResult Build(const FAkUGCProjectDocument& Document);

	/** 计算包内容确定性哈希（覆盖 Document 与 Logic IR）。 */
	static FString ComputeContentHash(
		const FAkUGCProjectDocument& Document,
		const FAkUGCLogicProgram& Program);
};

#pragma once

#include "CoreMinimal.h"
#include "Document/AkUGCDocument.h"
#include "AkUGCLogicPack.generated.h"

/**
 * Logic Pack 发布清单（Release Manifest）。
 *
 * 描述一个可发布、不可变、可信加载的作品包的元数据：
 *  - 发布标识与来源项目
 *  - Document Schema 版本（用于兼容性校验）
 *  - 能力与资产依赖（用于运行端预算与依赖校验）
 *  - 内容确定性哈希（用于完整性校验，配合后续的签名与验签）
 */
USTRUCT(BlueprintType)
struct AKUGCCORE_API FAkUGCLogicPackManifest
{
	GENERATED_BODY()

	/** 发布标识，同一作品内容修改后应重新生成。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC|Pack")
	FGuid ReleaseId;

	/** 打包时使用的 Document Schema 版本。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC|Pack")
	int32 SchemaVersion = AkUGCSchema::CurrentProjectDocumentVersion;

	/** 来源项目标识。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC|Pack")
	FGuid ProjectId;

	/** 来源模板。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC|Pack")
	FName TemplateId;

	/** 打包时声明的能力集合。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC|Pack")
	TArray<FName> Capabilities;

	/** 打包时解析出的资产依赖（Prefab 资产变体路径）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC|Pack")
	TArray<FString> AssetDependencies;

	/** 内容确定性哈希（SHA-256 hex）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC|Pack")
	FString ContentHash;
};

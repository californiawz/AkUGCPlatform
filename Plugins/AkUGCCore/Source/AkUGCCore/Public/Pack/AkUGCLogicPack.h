#pragma once

#include "CoreMinimal.h"
#include "Document/AkUGCDocument.h"
#include "Logic/AkUGCLogicCompiler.h"
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

/**
 * Ed25519 密钥对（原始 32 字节，hex 编码）。
 *
 * 私钥仅由发布方持有；公钥内置于运行端作为可信验签公钥。
 */
USTRUCT(BlueprintType)
struct AKUGCCORE_API FAkUGCLogicPackKeyPair
{
	GENERATED_BODY()

	/** 私钥（32 字节，hex 编码 64 字符）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC|Pack|Signature")
	FString PrivateKey;

	/** 公钥（32 字节，hex 编码 64 字符）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC|Pack|Signature")
	FString PublicKey;
};

/**
 * Logic Pack 数字签名。
 *
 * 对发布清单（Manifest）的确定性 JSON 做 Ed25519 签名：
 *  - Manifest 内含 ContentHash，而 ContentHash 覆盖 Document + Logic IR，
 *    因此签名传递性地覆盖整个发布包。
 *  - 验签时，签名内携带的公钥必须与运行端内置的可信公钥一致，
 *    防止攻击者用自己的私钥伪造「自洽」签名。
 */
USTRUCT(BlueprintType)
struct AKUGCCORE_API FAkUGCLogicPackSignature
{
	GENERATED_BODY()

	/** 签名算法标识，当前固定 "ed25519"。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC|Pack|Signature")
	FString Algorithm = TEXT("ed25519");

	/** 签发公钥（hex 编码），验签时需与可信公钥比对。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC|Pack|Signature")
	FString PublicKey;

	/** 签名值（64 字节，hex 编码 128 字符）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC|Pack|Signature")
	FString Signature;
};

/**
 * Logic Pack 发布包本体。
 *
 * 由发布清单、作者作品文档与编译后的 Logic IR 组成，
 * 是可发布、不可变、可信加载的最小单元。
 */
USTRUCT(BlueprintType)
struct AKUGCCORE_API FAkUGCLogicPack
{
	GENERATED_BODY()

	/** 发布清单（含内容确定性哈希）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC|Pack")
	FAkUGCLogicPackManifest Manifest;

	/** 作者作品文档（唯一真源）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC|Pack")
	FAkUGCProjectDocument Document;

	/** 编译后的 Logic IR。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC|Pack")
	FAkUGCLogicProgram Program;

	/** 数字签名（可选，空表示未签名）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UGC|Pack")
	FAkUGCLogicPackSignature Signature;
};

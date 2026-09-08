#pragma once

#include "CoreMinimal.h"
#include "Document/AkUGCDocument.h"
#include "Logic/AkUGCLogicCompiler.h"
#include "Pack/AkUGCLogicPack.h"

/**
 * Logic Pack 确定性哈希工具。
 *
 * 目标：对同一份逻辑内容，无论平台（Win64 / Android / Dedicated Server）或进程，
 * 都产生一致的 SHA-256 摘要，作为作品包的完整性校验基础。
 *
 * 确定性由「确定性 JSON 序列化」保证：
 *  - 对象字段按字段名字典序输出（消除 TMap 遍历顺序的不确定性）
 *  - 数值按 IEEE 754 位模式输出（消除跨平台十进制格式化差异）
 *  - 字符串按 UTF-8 字节转义输出（消除 TCHAR/编码差异）
 */
class AKUGCCORE_API FAkUGCLogicPackHasher
{
public:
	/** 对作品文档做确定性 JSON 序列化。 */
	static bool DeterministicSerialize(const FAkUGCProjectDocument& Document, FString& OutJson, FString* OutError = nullptr);

	/** 对编译后的 Logic IR 做确定性 JSON 序列化。 */
	static bool DeterministicSerializeProgram(const FAkUGCLogicProgram& Program, FString& OutJson, FString* OutError = nullptr);

	/** 对发布清单做确定性 JSON 序列化。 */
	static bool DeterministicSerializeManifest(const FAkUGCLogicPackManifest& Manifest, FString& OutJson, FString* OutError = nullptr);

	/** 计算作品文档的内容哈希（SHA-256 hex）。 */
	static FString HashDocument(const FAkUGCProjectDocument& Document, FString* OutError = nullptr);

	/** 计算 Logic IR 的内容哈希（SHA-256 hex）。 */
	static FString HashLogicProgram(const FAkUGCLogicProgram& Program, FString* OutError = nullptr);

	/** 计算发布清单的内容哈希（SHA-256 hex）。 */
	static FString HashManifest(const FAkUGCLogicPackManifest& Manifest, FString* OutError = nullptr);

	/** 底层：对 UTF-8 字节流计算 SHA-256 hex。 */
	static FString Sha256Hex(const FString& Utf8);
};

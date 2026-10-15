#pragma once

#include "CoreMinimal.h"
#include "Pack/AkUGCLogicPack.h"

/**
 * 发布身份的本机安全存储。
 *
 * Win64 使用当前 Windows 用户范围的 DPAPI 保护 Ed25519 私钥。调用方只能读取
 * 公钥或请求签名，不能从该接口导出私钥。旧版明文 private.key 会在确认身份一致后
 * 一次性迁移；损坏、冲突或无法解密时拒绝操作，绝不静默生成新身份。
 */
class AKUGCCORE_API FAkUGCPublisherKeyStore
{
public:
	explicit FAkUGCPublisherKeyStore(const FString& Directory);

	/** 默认发布身份目录：Saved/UGC/Publisher。 */
	static FString DefaultDirectory();

	/** 受保护身份文件路径。该文件仍是敏感凭据，不得分享或归档。 */
	FString IdentityPath() const;

	/**
	 * 旧版明文路径。迁移后保留不含秘密的兼容阻断标记，使旧版程序拒绝发布，
	 * 防止回滚时静默生成第二个作者身份。
	 */
	FString LegacyPrivateKeyPath() const;

	/** 创建或加载发布身份，只返回可公开核对的 Ed25519 公钥。 */
	bool GetOrCreatePublicKey(FString& OutPublicKey, FString* OutError = nullptr) const;

	/** 使用受保护身份为发布清单签名，不向调用方暴露私钥。 */
	bool Sign(
		const FAkUGCLogicPackManifest& Manifest,
		FAkUGCLogicPackSignature& OutSignature,
		FString* OutError = nullptr) const;

private:
	bool LoadOrCreate(
		TArray<uint8>& OutPrivateKey,
		FString& OutPublicKey,
		FString* OutError) const;

	FString Root;
};

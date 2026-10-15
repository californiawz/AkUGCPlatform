#pragma once

#include "CoreMinimal.h"
#include "Pack/AkUGCLogicPack.h"

/** 一次可验证的发布身份轮换。旧、新私钥都会签署同一份轮换声明。 */
struct AKUGCCORE_API FAkUGCPublisherKeyRotation
{
	int32 Sequence = 0;
	int64 CreatedAtUnixSeconds = 0;
	FString PreviousPublicKey;
	FString NewPublicKey;
	FString PreviousSignature;
	FString NewSignature;
};

/** 当前发布公钥及从初始公钥开始的完整轮换链；不包含私钥。 */
struct AKUGCCORE_API FAkUGCPublisherIdentityInfo
{
	int32 KeyVersion = 1;
	FString PublicKey;
	TArray<FAkUGCPublisherKeyRotation> Rotations;
};

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

	/** 创建或加载发布身份，返回不含秘密、可提交给 Registry 的身份历史。 */
	bool GetIdentityInfo(
		FAkUGCPublisherIdentityInfo& OutInfo,
		FString* OutError = nullptr) const;

	/** 使用受保护身份为发布清单签名，不向调用方暴露私钥。 */
	bool Sign(
		const FAkUGCLogicPackManifest& Manifest,
		FAkUGCLogicPackSignature& OutSignature,
		FString* OutError = nullptr) const;

	/**
	 * 导出跨 Windows 用户可恢复的密码加密备份。恢复包仍是敏感凭据：
	 * 不得提交仓库、归档到候选包或与密码放在同一位置。
	 */
	bool ExportRecoveryBundle(
		const FString& OutputPath,
		TConstArrayView<uint8> PassphraseUtf8,
		FString* OutError = nullptr) const;

	/**
	 * 将恢复包导入空身份目录；若现有身份完全相同则幂等成功。
	 * 不允许用恢复包静默覆盖另一身份或回退轮换历史。
	 */
	bool ImportRecoveryBundle(
		const FString& InputPath,
		TConstArrayView<uint8> PassphraseUtf8,
		FAkUGCPublisherIdentityInfo& OutInfo,
		FString* OutError = nullptr) const;

	/**
	 * 显式生成新身份并追加双签名轮换证明，同时为新身份导出恢复包。
	 * 恢复包写入失败时不会切换当前身份。
	 */
	bool RotateWithRecovery(
		const FString& RecoveryOutputPath,
		TConstArrayView<uint8> PassphraseUtf8,
		FAkUGCPublisherIdentityInfo& OutInfo,
		FString* OutError = nullptr) const;

	/** 验证轮换链连续性以及旧、新公钥的双签名。 */
	static bool VerifyIdentityHistory(
		const FAkUGCPublisherIdentityInfo& Info,
		FString* OutError = nullptr);

private:
	bool LoadOrCreate(
		TArray<uint8>& OutPrivateKey,
		FString& OutPublicKey,
		FAkUGCPublisherIdentityInfo* OutInfo,
		FString* OutError) const;

	bool LoadOrCreateLocked(
		TArray<uint8>& OutPrivateKey,
		FString& OutPublicKey,
		FAkUGCPublisherIdentityInfo* OutInfo,
		FString* OutError) const;

	FString Root;
};

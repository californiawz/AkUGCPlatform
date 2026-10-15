#pragma once

#include "CoreMinimal.h"
#include "Pack/AkUGCLogicPack.h"

class FAkUGCPublisherKeyStore;

/**
 * Logic Pack 签名器（发布方持有私钥）。
 *
 * 对发布清单（Manifest）的确定性 JSON 做 Ed25519 签名。
 * 由于 Manifest 内含 ContentHash（覆盖 Document + Logic IR），
 * 签名传递性地覆盖整个发布包，防止第三方伪造或篡改。
 */
class AKUGCCORE_API FAkUGCLogicPackSigner
{
public:
	/** 生成 Ed25519 密钥对（私钥/公钥，hex 编码）。 */
	static bool GenerateKeyPair(FAkUGCLogicPackKeyPair& OutKeyPair, FString* OutError = nullptr);

	/** 对发布清单签名，输出签名结构（含签发公钥）。 */
	static bool Sign(
		const FAkUGCLogicPackManifest& Manifest,
		const FString& PrivateKeyHex,
		FAkUGCLogicPackSignature& OutSignature,
		FString* OutError = nullptr);

private:
	friend class FAkUGCPublisherKeyStore;

	/** 仅供受保护密钥存储使用，避免将解密后的私钥转换为 FString。 */
	static bool SignPrivateKeyBytes(
		const FAkUGCLogicPackManifest& Manifest,
		TConstArrayView<uint8> PrivateKeyBytes,
		FAkUGCLogicPackSignature& OutSignature,
		FString* OutError = nullptr);

	/** 从原始 Ed25519 私钥导出公钥，用于迁移和身份完整性校验。 */
	static bool DerivePublicKey(
		TConstArrayView<uint8> PrivateKeyBytes,
		FString& OutPublicKey,
		FString* OutError = nullptr);
};

/**
 * Logic Pack 验签器（运行端持有可信公钥）。
 *
 * 用内置可信公钥验证清单签名：
 *  - 算法必须为 ed25519
 *  - 签名内携带的公钥必须与可信公钥一致
 *  - 对清单确定性 JSON 的 Ed25519 签名必须通过
 */
class AKUGCCORE_API FAkUGCLogicPackVerifier
{
public:
	/** 用可信公钥验证清单签名。 */
	static bool Verify(
		const FAkUGCLogicPackManifest& Manifest,
		const FAkUGCLogicPackSignature& Signature,
		const FString& TrustedPublicKeyHex,
		FString* OutError = nullptr);
};

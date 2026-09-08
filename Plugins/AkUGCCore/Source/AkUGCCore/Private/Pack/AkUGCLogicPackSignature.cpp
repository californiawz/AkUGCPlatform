#include "Pack/AkUGCLogicPackSignature.h"

#include "Containers/StringConv.h"
#include "Pack/AkUGCLogicPackHasher.h"

// OpenSSL 的 UI 类型别名与 UE 的 UI 命名空间冲突，include 期间重命名规避。
#define UI OpenSSL_UI
#include <openssl/evp.h>
#undef UI

namespace
{
	constexpr int32 Ed25519PrivateKeyBytes = 32;
	constexpr int32 Ed25519PublicKeyBytes = 32;
	constexpr int32 Ed25519SignatureBytes = 64;
	const TCHAR* Ed25519AlgorithmName = TEXT("ed25519");

	int32 HexNibble(TCHAR Character)
	{
		if (Character >= TEXT('0') && Character <= TEXT('9'))
		{
			return Character - TEXT('0');
		}
		if (Character >= TEXT('a') && Character <= TEXT('f'))
		{
			return Character - TEXT('a') + 10;
		}
		if (Character >= TEXT('A') && Character <= TEXT('F'))
		{
			return Character - TEXT('A') + 10;
		}
		return -1;
	}

	FString PackBytesToHex(const uint8* Data, int32 Length)
	{
		FString Hex;
		Hex.Reserve(Length * 2);
		for (int32 Index = 0; Index < Length; ++Index)
		{
			Hex += FString::Printf(TEXT("%02x"), static_cast<uint32>(Data[Index]));
		}
		return Hex;
	}

	bool HexToBytes(const FString& Hex, TArray<uint8>& Out)
	{
		Out.Reset();
		if ((Hex.Len() % 2) != 0)
		{
			return false;
		}
		Out.Reserve(Hex.Len() / 2);
		for (int32 Index = 0; Index < Hex.Len(); Index += 2)
		{
			const int32 High = HexNibble(Hex[Index]);
			const int32 Low = HexNibble(Hex[Index + 1]);
			if (High < 0 || Low < 0)
			{
				Out.Reset();
				return false;
			}
			Out.Add(static_cast<uint8>((High << 4) | Low));
		}
		return true;
	}

	/** 签名消息 = 发布清单的确定性 JSON（其内含 ContentHash，覆盖全部内容）。 */
	bool ComputeSigningMessage(const FAkUGCLogicPackManifest& Manifest, FString& OutMessage, FString& Error)
	{
		return FAkUGCLogicPackHasher::DeterministicSerializeManifest(Manifest, OutMessage, &Error);
	}
}

bool FAkUGCLogicPackSigner::GenerateKeyPair(FAkUGCLogicPackKeyPair& OutKeyPair, FString* OutError)
{
	EVP_PKEY_CTX* KeyCtx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
	if (KeyCtx == nullptr)
	{
		if (OutError)
		{
			*OutError = TEXT("签名初始化失败：无法创建 Ed25519 上下文。");
		}
		return false;
	}

	bool bSucceeded = false;
	EVP_PKEY* PKey = nullptr;
	do
	{
		if (EVP_PKEY_keygen_init(KeyCtx) <= 0)
		{
			if (OutError)
			{
				*OutError = TEXT("签名初始化失败：密钥生成初始化错误。");
			}
			break;
		}
		if (EVP_PKEY_keygen(KeyCtx, &PKey) <= 0)
		{
			if (OutError)
			{
				*OutError = TEXT("签名初始化失败：密钥生成错误。");
			}
			break;
		}

		uint8 PrivateKey[Ed25519PrivateKeyBytes];
		uint8 PublicKey[Ed25519PublicKeyBytes];
		size_t PrivateKeyLength = Ed25519PrivateKeyBytes;
		size_t PublicKeyLength = Ed25519PublicKeyBytes;
		if (EVP_PKEY_get_raw_private_key(PKey, PrivateKey, &PrivateKeyLength) <= 0
			|| EVP_PKEY_get_raw_public_key(PKey, PublicKey, &PublicKeyLength) <= 0)
		{
			if (OutError)
			{
				*OutError = TEXT("签名初始化失败：无法导出密钥。");
			}
			break;
		}

		OutKeyPair.PrivateKey = PackBytesToHex(PrivateKey, static_cast<int32>(PrivateKeyLength));
		OutKeyPair.PublicKey = PackBytesToHex(PublicKey, static_cast<int32>(PublicKeyLength));
		bSucceeded = true;
	} while (false);

	if (PKey != nullptr)
	{
		EVP_PKEY_free(PKey);
	}
	EVP_PKEY_CTX_free(KeyCtx);
	return bSucceeded;
}

bool FAkUGCLogicPackSigner::Sign(
	const FAkUGCLogicPackManifest& Manifest,
	const FString& PrivateKeyHex,
	FAkUGCLogicPackSignature& OutSignature,
	FString* OutError)
{
	TArray<uint8> PrivateKeyBytes;
	if (!HexToBytes(PrivateKeyHex, PrivateKeyBytes) || PrivateKeyBytes.Num() != Ed25519PrivateKeyBytes)
	{
		if (OutError)
		{
			*OutError = TEXT("签名失败：私钥格式非法（需 64 位 hex）。");
		}
		return false;
	}

	EVP_PKEY* PKey = EVP_PKEY_new_raw_private_key(
		EVP_PKEY_ED25519, nullptr, PrivateKeyBytes.GetData(), PrivateKeyBytes.Num());
	if (PKey == nullptr)
	{
		if (OutError)
		{
			*OutError = TEXT("签名失败：无法导入私钥。");
		}
		return false;
	}

	FString Message;
	FString MessageError;
	if (!ComputeSigningMessage(Manifest, Message, MessageError))
	{
		EVP_PKEY_free(PKey);
		if (OutError)
		{
			*OutError = FString::Printf(TEXT("签名失败：%s"), *MessageError);
		}
		return false;
	}

	FTCHARToUTF8 MessageUtf8(*Message, Message.Len());

	bool bSucceeded = false;
	EVP_MD_CTX* MdCtx = EVP_MD_CTX_new();
	if (MdCtx != nullptr)
	{
		if (EVP_DigestSignInit(MdCtx, nullptr, nullptr, nullptr, PKey) == 1)
		{
			size_t SignatureLength = 0;
			if (EVP_DigestSign(MdCtx, nullptr, &SignatureLength,
					reinterpret_cast<const uint8*>(MessageUtf8.Get()),
					static_cast<size_t>(MessageUtf8.Length())) == 1)
			{
				TArray<uint8> SignatureBytes;
				SignatureBytes.SetNumUninitialized(static_cast<int32>(SignatureLength));
				if (EVP_DigestSign(MdCtx, SignatureBytes.GetData(), &SignatureLength,
						reinterpret_cast<const uint8*>(MessageUtf8.Get()),
						static_cast<size_t>(MessageUtf8.Length())) == 1)
				{
					uint8 PublicKey[Ed25519PublicKeyBytes];
					size_t PublicKeyLength = Ed25519PublicKeyBytes;
					if (EVP_PKEY_get_raw_public_key(PKey, PublicKey, &PublicKeyLength) == 1)
					{
						OutSignature.Algorithm = Ed25519AlgorithmName;
						OutSignature.PublicKey = PackBytesToHex(PublicKey, static_cast<int32>(PublicKeyLength));
						OutSignature.Signature = PackBytesToHex(SignatureBytes.GetData(), static_cast<int32>(SignatureLength));
						bSucceeded = true;
					}
				}
			}
		}
		EVP_MD_CTX_free(MdCtx);
	}

	EVP_PKEY_free(PKey);

	if (!bSucceeded && OutError)
	{
		*OutError = TEXT("签名失败：Ed25519 签名计算错误。");
	}
	return bSucceeded;
}

bool FAkUGCLogicPackVerifier::Verify(
	const FAkUGCLogicPackManifest& Manifest,
	const FAkUGCLogicPackSignature& Signature,
	const FString& TrustedPublicKeyHex,
	FString* OutError)
{
	if (Signature.Algorithm != Ed25519AlgorithmName)
	{
		if (OutError)
		{
			*OutError = FString::Printf(TEXT("验签失败：不支持的签名算法 '%s'。"), *Signature.Algorithm);
		}
		return false;
	}

	if (Signature.PublicKey != TrustedPublicKeyHex)
	{
		if (OutError)
		{
			*OutError = TEXT("验签失败：签发公钥不受信任。");
		}
		return false;
	}

	TArray<uint8> SignatureBytes;
	if (!HexToBytes(Signature.Signature, SignatureBytes) || SignatureBytes.Num() != Ed25519SignatureBytes)
	{
		if (OutError)
		{
			*OutError = TEXT("验签失败：签名值格式非法（需 128 位 hex）。");
		}
		return false;
	}

	TArray<uint8> PublicKeyBytes;
	if (!HexToBytes(TrustedPublicKeyHex, PublicKeyBytes) || PublicKeyBytes.Num() != Ed25519PublicKeyBytes)
	{
		if (OutError)
		{
			*OutError = TEXT("验签失败：可信公钥格式非法（需 64 位 hex）。");
		}
		return false;
	}

	EVP_PKEY* PKey = EVP_PKEY_new_raw_public_key(
		EVP_PKEY_ED25519, nullptr, PublicKeyBytes.GetData(), PublicKeyBytes.Num());
	if (PKey == nullptr)
	{
		if (OutError)
		{
			*OutError = TEXT("验签失败：无法导入公钥。");
		}
		return false;
	}

	FString Message;
	FString MessageError;
	if (!ComputeSigningMessage(Manifest, Message, MessageError))
	{
		EVP_PKEY_free(PKey);
		if (OutError)
		{
			*OutError = FString::Printf(TEXT("验签失败：%s"), *MessageError);
		}
		return false;
	}

	FTCHARToUTF8 MessageUtf8(*Message, Message.Len());

	int32 VerifyResult = 0;
	EVP_MD_CTX* MdCtx = EVP_MD_CTX_new();
	if (MdCtx != nullptr)
	{
		if (EVP_DigestVerifyInit(MdCtx, nullptr, nullptr, nullptr, PKey) == 1)
		{
			VerifyResult = EVP_DigestVerify(MdCtx, SignatureBytes.GetData(),
				static_cast<size_t>(SignatureBytes.Num()),
				reinterpret_cast<const uint8*>(MessageUtf8.Get()),
				static_cast<size_t>(MessageUtf8.Length()));
		}
		EVP_MD_CTX_free(MdCtx);
	}

	EVP_PKEY_free(PKey);

	if (VerifyResult == 1)
	{
		return true;
	}
	if (OutError)
	{
		*OutError = TEXT("验签失败：签名不匹配，包可能已被篡改。");
	}
	return false;
}

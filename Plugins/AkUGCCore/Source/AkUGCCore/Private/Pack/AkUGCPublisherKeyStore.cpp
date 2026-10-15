#include "Pack/AkUGCPublisherKeyStore.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Pack/AkUGCLogicPackSignature.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Validation/AkUGCInputBudget.h"

// OpenSSL 的 UI 类型别名与 UE 的 UI 命名空间冲突，include 期间重命名规避。
#define UI OpenSSL_UI
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#undef UI

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <wincrypt.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace AkUGCPublisherKeyStorePrivate
{
	constexpr int32 LegacyPublisherIdentityVersion = 1;
	constexpr int32 PublisherIdentityVersion = 2;
	constexpr int32 RecoveryBundleVersion = 1;
	constexpr int32 Ed25519PrivateKeyBytes = 32;
	constexpr int32 Ed25519PublicKeyBytes = 32;
	constexpr int32 Ed25519SignatureBytes = 64;
	constexpr int32 RecoverySaltBytes = 16;
	constexpr int32 RecoveryNonceBytes = 12;
	constexpr int32 RecoveryKeyBytes = 32;
	constexpr int32 RecoveryTagBytes = 16;
	constexpr int32 MinRecoveryPassphraseBytes = 12;
	constexpr int32 MaxRecoveryPassphraseBytes = 1024;
	constexpr int32 MaxRotationRecords = 64;
	constexpr uint64 RecoveryScryptN = 131072;
	constexpr uint64 RecoveryScryptR = 8;
	constexpr uint64 RecoveryScryptP = 1;
	constexpr uint64 RecoveryScryptMaxMemory = 256ull * 1024ull * 1024ull;
	constexpr int64 MaxIdentityFileBytes = 256 * 1024;
	constexpr int64 MaxLegacyKeyFileBytes = 512;
	constexpr int64 MaxRecoveryBundleBytes = 512 * 1024;
	const TCHAR* IdentityFileName = TEXT("publisher-identity.json");
	const TCHAR* LegacyPrivateKeyFileName = TEXT("private.key");
	const TCHAR* LockFileName = TEXT("publisher-identity.lock");
	const TCHAR* ProtectionName = TEXT("windows-dpapi-current-user-v1");
	const TCHAR* RecoveryKind = TEXT("akugc-publisher-recovery");
	const TCHAR* RecoveryProtectionName = TEXT("scrypt-aes-256-gcm-v1");
	const TCHAR* RecoveryKdfName = TEXT("scrypt");
	const TCHAR* RecoveryCipherName = TEXT("aes-256-gcm");
	const TCHAR* LegacyMigrationFence =
		TEXT("{\"version\":1,\"migratedTo\":\"publisher-identity.json\",\"containsPrivateKey\":false}");

	enum class ELegacyIdentityState : uint8
	{
		Missing,
		Plaintext,
		MigrationFence,
	};

	bool Fail(FString* OutError, const FString& Message);
	bool PrivateKeysEqual(
		TConstArrayView<uint8> A,
		TConstArrayView<uint8> B);

#if PLATFORM_WINDOWS
	class FPublisherIdentityFileLock
	{
	public:
		~FPublisherIdentityFileLock()
		{
			if (Handle != INVALID_HANDLE_VALUE)
			{
				CloseHandle(Handle);
			}
		}

		bool Acquire(const FString& Root, FString* OutError)
		{
			if (!IFileManager::Get().MakeDirectory(*Root, true))
			{
				return Fail(OutError, TEXT("无法创建发布身份目录。"));
			}
			const FString LockPath = FPaths::Combine(Root, LockFileName);
			const double Deadline = FPlatformTime::Seconds() + 10.0;
			do
			{
				Handle = CreateFileW(
					*LockPath,
					GENERIC_READ | GENERIC_WRITE,
					0,
					nullptr,
					OPEN_ALWAYS,
					FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED,
					nullptr);
				if (Handle != INVALID_HANDLE_VALUE)
				{
					return true;
				}
				const uint32 ErrorCode = static_cast<uint32>(GetLastError());
				if (ErrorCode != ERROR_SHARING_VIOLATION
					&& ErrorCode != ERROR_LOCK_VIOLATION)
				{
					return Fail(
						OutError,
						FString::Printf(TEXT("无法锁定发布身份存储（错误 %u）。"), ErrorCode));
				}
				FPlatformProcess::SleepNoStats(0.01f);
			} while (FPlatformTime::Seconds() < Deadline);
			return Fail(OutError, TEXT("等待发布身份存储锁超时。"));
		}

	private:
		HANDLE Handle = INVALID_HANDLE_VALUE;
	};
#endif

	bool Fail(FString* OutError, const FString& Message)
	{
		if (OutError)
		{
			*OutError = Message;
		}
		return false;
	}

	void SecureReset(TArray<uint8>& Value)
	{
		if (Value.Num() > 0)
		{
			FMemory::Memzero(Value.GetData(), Value.Num());
		}
		Value.Reset();
	}

	void SecureReset(FString& Value)
	{
		TArray<TCHAR>& Characters = Value.GetCharArray();
		if (Characters.Num() > 0)
		{
			FMemory::Memzero(Characters.GetData(), Characters.Num() * sizeof(TCHAR));
		}
		Value.Reset();
	}

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

	FString BytesToHex(TConstArrayView<uint8> Bytes)
	{
		FString Hex;
		Hex.Reserve(Bytes.Num() * 2);
		for (const uint8 Byte : Bytes)
		{
			Hex += FString::Printf(TEXT("%02x"), static_cast<uint32>(Byte));
		}
		return Hex;
	}

	bool HexToBytes(const FString& Hex, int32 ExpectedBytes, TArray<uint8>& OutBytes)
	{
		OutBytes.Reset();
		if (ExpectedBytes <= 0 || Hex.Len() != ExpectedBytes * 2)
		{
			return false;
		}
		OutBytes.SetNumUninitialized(ExpectedBytes);
		for (int32 Index = 0; Index < ExpectedBytes; ++Index)
		{
			const int32 High = HexNibble(Hex[Index * 2]);
			const int32 Low = HexNibble(Hex[Index * 2 + 1]);
			if (High < 0 || Low < 0)
			{
				SecureReset(OutBytes);
				return false;
			}
			OutBytes[Index] = static_cast<uint8>((High << 4) | Low);
		}
		return true;
	}

	bool HexToPrivateKeyBytes(const FString& Hex, TArray<uint8>& OutPrivateKey)
	{
		return HexToBytes(Hex, Ed25519PrivateKeyBytes, OutPrivateKey);
	}

	bool IsCanonicalHex(const FString& Value, int32 Bytes)
	{
		if (Value.Len() != Bytes * 2 || Value != Value.ToLower())
		{
			return false;
		}
		for (const TCHAR Character : Value)
		{
			if (HexNibble(Character) < 0)
			{
				return false;
			}
		}
		return true;
	}

	bool TryGetExactInt32(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field,
		int32& OutValue)
	{
		double Number = 0.0;
		if (!Object.IsValid()
			|| !Object->TryGetNumberField(Field, Number)
			|| !FMath::IsFinite(Number)
			|| Number < static_cast<double>(MIN_int32)
			|| Number > static_cast<double>(MAX_int32)
			|| Number != FMath::TruncToDouble(Number))
		{
			return false;
		}
		OutValue = static_cast<int32>(Number);
		return true;
	}

	bool TryGetExactInt64(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field,
		int64& OutValue)
	{
		double Number = 0.0;
		if (!Object.IsValid()
			|| !Object->TryGetNumberField(Field, Number)
			|| !FMath::IsFinite(Number)
			|| Number < 0.0
			|| Number > 9007199254740991.0
			|| Number != FMath::TruncToDouble(Number))
		{
			return false;
		}
		OutValue = static_cast<int64>(Number);
		return true;
	}

	bool DecodeCanonicalBase64(
		const FString& Encoded,
		int32 ExpectedBytes,
		TArray<uint8>& OutBytes)
	{
		OutBytes.Reset();
		if (Encoded.IsEmpty()
			|| Encoded.Len() > ((ExpectedBytes + 2) / 3) * 4
			|| !FBase64::Decode(Encoded, OutBytes)
			|| OutBytes.Num() != ExpectedBytes
			|| FBase64::Encode(OutBytes.GetData(), OutBytes.Num()) != Encoded)
		{
			SecureReset(OutBytes);
			return false;
		}
		return true;
	}

	bool ReadBoundedText(
		const FString& Path,
		int64 MaxBytes,
		FString& OutText,
		FString* OutError,
		const TCHAR* Label)
	{
		return AkUGCInputBudget::LoadTextFile(
			Path,
			MaxBytes,
			Label,
			OutText,
			OutError);
	}

	FString RotationMessage(const FAkUGCPublisherKeyRotation& Rotation)
	{
		return FString::Printf(
			TEXT("AkUGCPlatform.PublisherRotation.v1\n")
			TEXT("sequence=%d\n")
			TEXT("createdAtUnixSeconds=%lld\n")
			TEXT("previousPublicKey=%s\n")
			TEXT("newPublicKey=%s"),
			Rotation.Sequence,
			static_cast<long long>(Rotation.CreatedAtUnixSeconds),
			*Rotation.PreviousPublicKey,
			*Rotation.NewPublicKey);
	}

	bool DerivePublicKeyRaw(
		TConstArrayView<uint8> PrivateKey,
		FString& OutPublicKey,
		FString* OutError)
	{
		OutPublicKey.Reset();
		if (PrivateKey.Num() != Ed25519PrivateKeyBytes)
		{
			return Fail(OutError, TEXT("发布私钥长度非法，无法导出公钥。"));
		}
		EVP_PKEY* Key = EVP_PKEY_new_raw_private_key(
			EVP_PKEY_ED25519,
			nullptr,
			PrivateKey.GetData(),
			static_cast<size_t>(PrivateKey.Num()));
		uint8 PublicKey[Ed25519PublicKeyBytes] = {};
		size_t PublicKeyLength = sizeof(PublicKey);
		const bool bDerived = Key
			&& EVP_PKEY_get_raw_public_key(
				Key,
				PublicKey,
				&PublicKeyLength) == 1
			&& PublicKeyLength == Ed25519PublicKeyBytes;
		if (Key)
		{
			EVP_PKEY_free(Key);
		}
		if (!bDerived)
		{
			return Fail(OutError, TEXT("无法从发布私钥导出公钥。"));
		}
		OutPublicKey = BytesToHex(
			MakeArrayView(PublicKey, Ed25519PublicKeyBytes));
		return true;
	}

	bool SignMessage(
		TConstArrayView<uint8> PrivateKey,
		const FString& Message,
		FString& OutSignature,
		FString* OutError)
	{
		OutSignature.Reset();
		if (PrivateKey.Num() != Ed25519PrivateKeyBytes)
		{
			return Fail(OutError, TEXT("轮换签名私钥长度非法。"));
		}
		EVP_PKEY* Key = EVP_PKEY_new_raw_private_key(
			EVP_PKEY_ED25519,
			nullptr,
			PrivateKey.GetData(),
			static_cast<size_t>(PrivateKey.Num()));
		EVP_MD_CTX* Context = Key ? EVP_MD_CTX_new() : nullptr;
		uint8 Signature[Ed25519SignatureBytes] = {};
		size_t SignatureLength = sizeof(Signature);
		const FTCHARToUTF8 MessageUtf8(*Message);
		const bool bSigned = Context
			&& EVP_DigestSignInit(Context, nullptr, nullptr, nullptr, Key) == 1
			&& EVP_DigestSign(
				Context,
				Signature,
				&SignatureLength,
				reinterpret_cast<const uint8*>(MessageUtf8.Get()),
				static_cast<size_t>(MessageUtf8.Length())) == 1
			&& SignatureLength == Ed25519SignatureBytes;
		if (Context)
		{
			EVP_MD_CTX_free(Context);
		}
		if (Key)
		{
			EVP_PKEY_free(Key);
		}
		if (!bSigned)
		{
			FMemory::Memzero(Signature, sizeof(Signature));
			return Fail(OutError, TEXT("无法生成发布身份轮换签名。"));
		}
		OutSignature = BytesToHex(MakeArrayView(Signature, Ed25519SignatureBytes));
		FMemory::Memzero(Signature, sizeof(Signature));
		return true;
	}

	bool VerifyMessage(
		const FString& PublicKeyHex,
		const FString& Message,
		const FString& SignatureHex)
	{
		TArray<uint8> PublicKey;
		TArray<uint8> Signature;
		if (!HexToBytes(PublicKeyHex, Ed25519PublicKeyBytes, PublicKey)
			|| !HexToBytes(SignatureHex, Ed25519SignatureBytes, Signature))
		{
			return false;
		}
		EVP_PKEY* Key = EVP_PKEY_new_raw_public_key(
			EVP_PKEY_ED25519,
			nullptr,
			PublicKey.GetData(),
			static_cast<size_t>(PublicKey.Num()));
		EVP_MD_CTX* Context = Key ? EVP_MD_CTX_new() : nullptr;
		const FTCHARToUTF8 MessageUtf8(*Message);
		const bool bVerified = Context
			&& EVP_DigestVerifyInit(Context, nullptr, nullptr, nullptr, Key) == 1
			&& EVP_DigestVerify(
				Context,
				Signature.GetData(),
				static_cast<size_t>(Signature.Num()),
				reinterpret_cast<const uint8*>(MessageUtf8.Get()),
				static_cast<size_t>(MessageUtf8.Length())) == 1;
		if (Context)
		{
			EVP_MD_CTX_free(Context);
		}
		if (Key)
		{
			EVP_PKEY_free(Key);
		}
		return bVerified;
	}

	bool VerifyIdentityHistoryInternal(
		const FAkUGCPublisherIdentityInfo& Info,
		FString* OutError)
	{
		if (Info.KeyVersion < 1
			|| Info.Rotations.Num() != Info.KeyVersion - 1
			|| Info.Rotations.Num() > MaxRotationRecords
			|| !IsCanonicalHex(Info.PublicKey, Ed25519PublicKeyBytes))
		{
			return Fail(OutError, TEXT("发布身份历史版本、数量或当前公钥非法。"));
		}

		FString ExpectedPreviousKey =
			Info.Rotations.IsEmpty()
				? Info.PublicKey
				: Info.Rotations[0].PreviousPublicKey;
		for (int32 Index = 0; Index < Info.Rotations.Num(); ++Index)
		{
			const FAkUGCPublisherKeyRotation& Rotation = Info.Rotations[Index];
			if (Rotation.Sequence != Index + 1
				|| Rotation.CreatedAtUnixSeconds <= 0
				|| !IsCanonicalHex(Rotation.PreviousPublicKey, Ed25519PublicKeyBytes)
				|| !IsCanonicalHex(Rotation.NewPublicKey, Ed25519PublicKeyBytes)
				|| !IsCanonicalHex(Rotation.PreviousSignature, Ed25519SignatureBytes)
				|| !IsCanonicalHex(Rotation.NewSignature, Ed25519SignatureBytes)
				|| Rotation.PreviousPublicKey != ExpectedPreviousKey
				|| Rotation.NewPublicKey == Rotation.PreviousPublicKey)
			{
				return Fail(OutError, TEXT("发布身份轮换链不连续或字段非法。"));
			}
			const FString Message = RotationMessage(Rotation);
			if (!VerifyMessage(
					Rotation.PreviousPublicKey,
					Message,
					Rotation.PreviousSignature)
				|| !VerifyMessage(
					Rotation.NewPublicKey,
					Message,
					Rotation.NewSignature))
			{
				return Fail(OutError, TEXT("发布身份轮换双签名校验失败。"));
			}
			ExpectedPreviousKey = Rotation.NewPublicKey;
		}
		if (ExpectedPreviousKey != Info.PublicKey)
		{
			return Fail(OutError, TEXT("发布身份历史终点与当前公钥不一致。"));
		}
		return true;
	}

	TArray<TSharedPtr<FJsonValue>> SerializeRotations(
		const TArray<FAkUGCPublisherKeyRotation>& Rotations)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		Values.Reserve(Rotations.Num());
		for (const FAkUGCPublisherKeyRotation& Rotation : Rotations)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetNumberField(TEXT("sequence"), Rotation.Sequence);
			Object->SetNumberField(
				TEXT("createdAtUnixSeconds"),
				static_cast<double>(Rotation.CreatedAtUnixSeconds));
			Object->SetStringField(
				TEXT("previousPublicKey"),
				Rotation.PreviousPublicKey);
			Object->SetStringField(TEXT("newPublicKey"), Rotation.NewPublicKey);
			Object->SetStringField(
				TEXT("previousSignature"),
				Rotation.PreviousSignature);
			Object->SetStringField(TEXT("newSignature"), Rotation.NewSignature);
			Values.Add(MakeShared<FJsonValueObject>(Object));
		}
		return Values;
	}

	bool ParseRotations(
		const TSharedPtr<FJsonObject>& Object,
		TArray<FAkUGCPublisherKeyRotation>& OutRotations,
		FString* OutError)
	{
		OutRotations.Reset();
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Object.IsValid()
			|| !Object->TryGetArrayField(TEXT("rotations"), Values)
			|| !Values
			|| Values->Num() > MaxRotationRecords)
		{
			return Fail(OutError, TEXT("发布身份轮换历史缺失或超过数量限制。"));
		}
		OutRotations.Reserve(Values->Num());
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			const TSharedPtr<FJsonObject> RotationObject =
				Value.IsValid() ? Value->AsObject() : nullptr;
			FAkUGCPublisherKeyRotation Rotation;
			if (!RotationObject.IsValid()
				|| RotationObject->Values.Num() != 6
				|| !TryGetExactInt32(
					RotationObject,
					TEXT("sequence"),
					Rotation.Sequence)
				|| !TryGetExactInt64(
					RotationObject,
					TEXT("createdAtUnixSeconds"),
					Rotation.CreatedAtUnixSeconds)
				|| !RotationObject->TryGetStringField(
					TEXT("previousPublicKey"),
					Rotation.PreviousPublicKey)
				|| !RotationObject->TryGetStringField(
					TEXT("newPublicKey"),
					Rotation.NewPublicKey)
				|| !RotationObject->TryGetStringField(
					TEXT("previousSignature"),
					Rotation.PreviousSignature)
				|| !RotationObject->TryGetStringField(
					TEXT("newSignature"),
					Rotation.NewSignature))
			{
				OutRotations.Reset();
				return Fail(OutError, TEXT("发布身份轮换历史格式错误。"));
			}
			OutRotations.Add(MoveTemp(Rotation));
		}
		return true;
	}

	bool IdentityInfoEqual(
		const FAkUGCPublisherIdentityInfo& A,
		const FAkUGCPublisherIdentityInfo& B)
	{
		if (A.KeyVersion != B.KeyVersion
			|| A.PublicKey != B.PublicKey
			|| A.Rotations.Num() != B.Rotations.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < A.Rotations.Num(); ++Index)
		{
			const FAkUGCPublisherKeyRotation& Left = A.Rotations[Index];
			const FAkUGCPublisherKeyRotation& Right = B.Rotations[Index];
			if (Left.Sequence != Right.Sequence
				|| Left.CreatedAtUnixSeconds != Right.CreatedAtUnixSeconds
				|| Left.PreviousPublicKey != Right.PreviousPublicKey
				|| Left.NewPublicKey != Right.NewPublicKey
				|| Left.PreviousSignature != Right.PreviousSignature
				|| Left.NewSignature != Right.NewSignature)
			{
				return false;
			}
		}
		return true;
	}

	bool InspectLegacyIdentity(
		const FString& Path,
		ELegacyIdentityState& OutState,
		TArray<uint8>& OutPrivateKey,
		FString* OutError)
	{
		SecureReset(OutPrivateKey);
		if (!IFileManager::Get().FileExists(*Path))
		{
			OutState = ELegacyIdentityState::Missing;
			return true;
		}
		FString PrivateKeyHex;
		if (!ReadBoundedText(
				Path,
				MaxLegacyKeyFileBytes,
				PrivateKeyHex,
				OutError,
				TEXT("旧版发布私钥")))
		{
			return false;
		}
		if (PrivateKeyHex == LegacyMigrationFence)
		{
			OutState = ELegacyIdentityState::MigrationFence;
			SecureReset(PrivateKeyHex);
			return true;
		}
		PrivateKeyHex.TrimStartAndEndInline();
		const bool bValid = HexToPrivateKeyBytes(PrivateKeyHex, OutPrivateKey);
		SecureReset(PrivateKeyHex);
		if (!bValid)
		{
			return Fail(OutError, TEXT("旧版发布私钥格式错误，禁止迁移或自动替换。"));
		}
		OutState = ELegacyIdentityState::Plaintext;
		return true;
	}

	bool WriteMigrationFence(
		const FString& Path,
		bool bReplaceExisting,
		FString* OutError)
	{
		if (IFileManager::Get().DirectoryExists(*Path))
		{
			return Fail(OutError, TEXT("旧版发布私钥路径被目录占用。"));
		}
		const FString Temporary =
			Path + TEXT(".") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".tmp");
		const bool bWritten =
			FFileHelper::SaveStringToFile(
				LegacyMigrationFence,
				*Temporary,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
			&& IFileManager::Get().Move(
				*Path,
				*Temporary,
				bReplaceExisting,
				false,
				false,
				true);
		if (!bWritten)
		{
			IFileManager::Get().Delete(*Temporary, false, false, true);
			return Fail(OutError, TEXT("无法写入旧版发布程序兼容阻断标记。"));
		}
		FString Stored;
		if (!ReadBoundedText(
				Path,
				MaxLegacyKeyFileBytes,
				Stored,
				OutError,
				TEXT("发布身份兼容阻断标记"))
			|| Stored != LegacyMigrationFence)
		{
			SecureReset(Stored);
			return Fail(OutError, TEXT("发布身份兼容阻断标记写入后校验失败。"));
		}
		SecureReset(Stored);
		return true;
	}

	bool RejectSensitiveTemporaryFiles(
		const FString& Root,
		FString* OutError)
	{
		TArray<FString> Files;
		IFileManager::Get().FindFiles(
			Files,
			*FPaths::Combine(Root, TEXT("publisher-identity.json.*.tmp")),
			true,
			false);
		TArray<FString> LegacyFiles;
		IFileManager::Get().FindFiles(
			LegacyFiles,
			*FPaths::Combine(Root, TEXT("private.key.*.tmp")),
			true,
			false);
		Files.Append(LegacyFiles);
		if (!Files.IsEmpty())
		{
			Files.Sort();
			return Fail(
				OutError,
				TEXT("检测到未完成的敏感发布身份临时文件；请先隔离并人工处理：")
					+ FString::Join(Files, TEXT(", ")));
		}
		return true;
	}

	FString EntropyForPublicKey(const FString& PublicKey)
	{
		return TEXT("AkUGCPlatform.PublisherIdentity.v1|") + PublicKey;
	}

	bool ProtectPrivateKey(
		TConstArrayView<uint8> PrivateKey,
		const FString& PublicKey,
		TArray<uint8>& OutProtected,
		FString* OutError)
	{
		OutProtected.Reset();
#if PLATFORM_WINDOWS
		if (PrivateKey.Num() != Ed25519PrivateKeyBytes)
		{
			return Fail(OutError, TEXT("发布私钥长度非法，无法保护。"));
		}

		const FString EntropyText = EntropyForPublicKey(PublicKey);
		FTCHARToUTF8 EntropyUtf8(*EntropyText);
		DATA_BLOB Input;
		Input.cbData = static_cast<DWORD>(PrivateKey.Num());
		Input.pbData = const_cast<BYTE*>(reinterpret_cast<const BYTE*>(PrivateKey.GetData()));
		DATA_BLOB Entropy;
		Entropy.cbData = static_cast<DWORD>(EntropyUtf8.Length());
		Entropy.pbData = reinterpret_cast<BYTE*>(const_cast<ANSICHAR*>(EntropyUtf8.Get()));
		DATA_BLOB Protected = {};

		if (!CryptProtectData(
				&Input,
				TEXT("AkUGCPlatform Publisher Identity"),
				&Entropy,
				nullptr,
				nullptr,
				CRYPTPROTECT_UI_FORBIDDEN,
				&Protected))
		{
			const uint32 ErrorCode = static_cast<uint32>(GetLastError());
			return Fail(
				OutError,
				FString::Printf(TEXT("Windows DPAPI 无法保护发布私钥（错误 %u）。"), ErrorCode));
		}

		OutProtected.Append(Protected.pbData, static_cast<int32>(Protected.cbData));
		if (Protected.pbData && Protected.cbData > 0)
		{
			SecureZeroMemory(Protected.pbData, Protected.cbData);
		}
		LocalFree(Protected.pbData);
		return OutProtected.Num() > 0;
#else
		return Fail(OutError, TEXT("当前平台不支持创建发布身份；P18 仅启用 Win64 DPAPI。"));
#endif
	}

	bool UnprotectPrivateKey(
		TConstArrayView<uint8> Protected,
		const FString& PublicKey,
		TArray<uint8>& OutPrivateKey,
		FString* OutError)
	{
		OutPrivateKey.Reset();
#if PLATFORM_WINDOWS
		if (Protected.IsEmpty())
		{
			return Fail(OutError, TEXT("受保护发布私钥为空。"));
		}

		const FString EntropyText = EntropyForPublicKey(PublicKey);
		FTCHARToUTF8 EntropyUtf8(*EntropyText);
		DATA_BLOB Input;
		Input.cbData = static_cast<DWORD>(Protected.Num());
		Input.pbData = const_cast<BYTE*>(reinterpret_cast<const BYTE*>(Protected.GetData()));
		DATA_BLOB Entropy;
		Entropy.cbData = static_cast<DWORD>(EntropyUtf8.Length());
		Entropy.pbData = reinterpret_cast<BYTE*>(const_cast<ANSICHAR*>(EntropyUtf8.Get()));
		DATA_BLOB Plaintext = {};
		LPWSTR Description = nullptr;

		if (!CryptUnprotectData(
				&Input,
				&Description,
				&Entropy,
				nullptr,
				nullptr,
				CRYPTPROTECT_UI_FORBIDDEN,
				&Plaintext))
		{
			const uint32 ErrorCode = static_cast<uint32>(GetLastError());
			if (Description)
			{
				LocalFree(Description);
			}
			return Fail(
				OutError,
				FString::Printf(
					TEXT("发布身份无法由当前 Windows 用户解密（错误 %u）；禁止自动换钥。"),
					ErrorCode));
		}

		OutPrivateKey.Append(Plaintext.pbData, static_cast<int32>(Plaintext.cbData));
		if (Plaintext.pbData && Plaintext.cbData > 0)
		{
			SecureZeroMemory(Plaintext.pbData, Plaintext.cbData);
		}
		LocalFree(Plaintext.pbData);
		if (Description)
		{
			LocalFree(Description);
		}
		if (OutPrivateKey.Num() != Ed25519PrivateKeyBytes)
		{
			SecureReset(OutPrivateKey);
			return Fail(OutError, TEXT("解密后的发布私钥长度非法，禁止自动换钥。"));
		}
		return true;
#else
		return Fail(OutError, TEXT("当前平台不支持读取发布身份；P18 仅启用 Win64 DPAPI。"));
#endif
	}

	bool SerializeIdentity(
		const FAkUGCPublisherIdentityInfo& Info,
		TConstArrayView<uint8> ProtectedPrivateKey,
		FString& OutJson,
		FString* OutError)
	{
		if (!VerifyIdentityHistoryInternal(Info, OutError))
		{
			return false;
		}
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetNumberField(TEXT("version"), PublisherIdentityVersion);
		Object->SetStringField(TEXT("protection"), ProtectionName);
		Object->SetNumberField(TEXT("keyVersion"), Info.KeyVersion);
		Object->SetStringField(TEXT("publicKey"), Info.PublicKey);
		Object->SetStringField(
			TEXT("protectedPrivateKey"),
			FBase64::Encode(ProtectedPrivateKey.GetData(), ProtectedPrivateKey.Num()));
		Object->SetArrayField(TEXT("rotations"), SerializeRotations(Info.Rotations));
		OutJson.Reset();
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutJson);
		if (!FJsonSerializer::Serialize(Object, Writer))
		{
			return Fail(OutError, TEXT("无法序列化受保护发布身份。"));
		}
		return true;
	}

	bool ParseIdentity(
		const FString& Json,
		FAkUGCPublisherIdentityInfo& OutInfo,
		TArray<uint8>& OutProtectedPrivateKey,
		FString* OutError)
	{
		OutInfo = FAkUGCPublisherIdentityInfo();
		OutProtectedPrivateKey.Reset();
		if (!AkUGCInputBudget::ValidateJson(
				Json,
				MaxIdentityFileBytes,
				TEXT("Protected publisher identity"),
				OutError))
		{
			return false;
		}
		TSharedPtr<FJsonObject> Object;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Object)
			|| !Object.IsValid())
		{
			return Fail(OutError, TEXT("受保护发布身份格式错误，禁止自动替换。"));
		}

		int32 Version = 0;
		FString Protection;
		FString EncodedPrivateKey;
		if (!TryGetExactInt32(Object, TEXT("version"), Version)
			|| (Version != LegacyPublisherIdentityVersion
				&& Version != PublisherIdentityVersion)
			|| Object->Values.Num()
				!= (Version == LegacyPublisherIdentityVersion ? 4 : 6)
			|| !Object->TryGetStringField(TEXT("protection"), Protection)
			|| Protection != ProtectionName
			|| !Object->TryGetStringField(TEXT("publicKey"), OutInfo.PublicKey)
			|| !Object->TryGetStringField(TEXT("protectedPrivateKey"), EncodedPrivateKey)
			|| !IsCanonicalHex(OutInfo.PublicKey, Ed25519PublicKeyBytes))
		{
			OutInfo = FAkUGCPublisherIdentityInfo();
			return Fail(OutError, TEXT("受保护发布身份字段错误或版本不兼容，禁止自动替换。"));
		}

		if (Version == PublisherIdentityVersion)
		{
			if (!TryGetExactInt32(
					Object,
					TEXT("keyVersion"),
					OutInfo.KeyVersion)
				|| !ParseRotations(Object, OutInfo.Rotations, OutError))
			{
				OutInfo = FAkUGCPublisherIdentityInfo();
				return false;
			}
		}
		else
		{
			OutInfo.KeyVersion = 1;
			OutInfo.Rotations.Reset();
		}
		if (!FBase64::Decode(EncodedPrivateKey, OutProtectedPrivateKey)
			|| OutProtectedPrivateKey.IsEmpty()
			|| OutProtectedPrivateKey.Num() > MaxIdentityFileBytes
			|| FBase64::Encode(
				OutProtectedPrivateKey.GetData(),
				OutProtectedPrivateKey.Num()) != EncodedPrivateKey)
		{
			OutInfo = FAkUGCPublisherIdentityInfo();
			OutProtectedPrivateKey.Reset();
			return Fail(OutError, TEXT("受保护发布身份的密文格式错误。"));
		}
		if (!VerifyIdentityHistoryInternal(OutInfo, OutError))
		{
			OutInfo = FAkUGCPublisherIdentityInfo();
			SecureReset(OutProtectedPrivateKey);
			return false;
		}
		return true;
	}

	bool LoadIdentityFile(
		const FString& Path,
		TArray<uint8>& OutPrivateKey,
		FAkUGCPublisherIdentityInfo& OutInfo,
		FString* OutError)
	{
		FString Json;
		if (!ReadBoundedText(
				Path,
				MaxIdentityFileBytes,
				Json,
				OutError,
				TEXT("受保护发布身份")))
		{
			return false;
		}

		TArray<uint8> ProtectedPrivateKey;
		if (!ParseIdentity(Json, OutInfo, ProtectedPrivateKey, OutError))
		{
			return false;
		}
		const bool bUnprotected = UnprotectPrivateKey(
			ProtectedPrivateKey,
			OutInfo.PublicKey,
			OutPrivateKey,
			OutError);
		SecureReset(ProtectedPrivateKey);
		if (!bUnprotected)
		{
			OutInfo = FAkUGCPublisherIdentityInfo();
		}
		return bUnprotected;
	}

	bool LoadIdentityMetadata(
		const FString& Path,
		FAkUGCPublisherIdentityInfo& OutInfo,
		FString* OutError)
	{
		FString Json;
		if (!ReadBoundedText(
				Path,
				MaxIdentityFileBytes,
				Json,
				OutError,
				TEXT("受保护发布身份")))
		{
			return false;
		}
		TArray<uint8> ProtectedPrivateKey;
		const bool bParsed = ParseIdentity(
			Json,
			OutInfo,
			ProtectedPrivateKey,
			OutError);
		SecureReset(ProtectedPrivateKey);
		return bParsed;
	}

	bool WriteIdentityFile(
		const FString& Path,
		TConstArrayView<uint8> PrivateKey,
		const FAkUGCPublisherIdentityInfo& Info,
		bool bReplaceExisting,
		FString* OutError)
	{
		const bool bExists = IFileManager::Get().FileExists(*Path);
		if (IFileManager::Get().DirectoryExists(*Path)
			|| (bExists && !bReplaceExisting)
			|| (!bExists && bReplaceExisting))
		{
			return Fail(
				OutError,
				bReplaceExisting
					? TEXT("待轮换的受保护发布身份不存在或路径异常。")
					: TEXT("受保护发布身份已存在，不会覆盖。"));
		}

		TArray<uint8> ProtectedPrivateKey;
		if (!ProtectPrivateKey(
				PrivateKey,
				Info.PublicKey,
				ProtectedPrivateKey,
				OutError))
		{
			return false;
		}
		FString Json;
		const bool bSerialized = SerializeIdentity(
			Info,
			ProtectedPrivateKey,
			Json,
			OutError);
		SecureReset(ProtectedPrivateKey);
		if (!bSerialized)
		{
			return false;
		}
		if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true))
		{
			return Fail(OutError, TEXT("无法创建发布身份目录。"));
		}

		const FString Temporary =
			Path + TEXT(".") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".tmp");
		const bool bWritten =
			FFileHelper::SaveStringToFile(
				Json,
				*Temporary,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
			;
		if (!bWritten)
		{
			IFileManager::Get().Delete(*Temporary, false, false, true);
			return Fail(OutError, TEXT("受保护发布身份临时文件写入失败。"));
		}

		TArray<uint8> VerifiedPrivateKey;
		FAkUGCPublisherIdentityInfo VerifiedInfo;
		FString VerificationError;
		const bool bVerified =
			LoadIdentityFile(
				Temporary,
				VerifiedPrivateKey,
				VerifiedInfo,
				&VerificationError)
			&& PrivateKeysEqual(VerifiedPrivateKey, PrivateKey)
			&& IdentityInfoEqual(VerifiedInfo, Info);
		SecureReset(VerifiedPrivateKey);
		if (!bVerified)
		{
			IFileManager::Get().Delete(*Temporary, false, false, true);
			return Fail(
				OutError,
				TEXT("受保护发布身份临时文件回读校验失败：")
					+ VerificationError);
		}

		const bool bMoved = IFileManager::Get().Move(
			*Path,
			*Temporary,
			bReplaceExisting,
			false,
			false,
			true);
		if (!bMoved)
		{
			IFileManager::Get().Delete(*Temporary, false, false, true);
			return Fail(OutError, TEXT("受保护发布身份原子写入失败。"));
		}
		return true;
	}

	bool PrivateKeysEqual(
		TConstArrayView<uint8> A,
		TConstArrayView<uint8> B)
	{
		return A.Num() == B.Num()
			&& A.Num() == Ed25519PrivateKeyBytes
			&& CRYPTO_memcmp(A.GetData(), B.GetData(), A.Num()) == 0;
	}

	struct FRecoveryBundleFields
	{
		FAkUGCPublisherIdentityInfo Info;
		TArray<uint8> Salt;
		TArray<uint8> Nonce;
		TArray<uint8> Ciphertext;
		TArray<uint8> Tag;
	};

	bool ValidateRecoveryPassphrase(
		TConstArrayView<uint8> Passphrase,
		FString* OutError)
	{
		if (Passphrase.Num() < MinRecoveryPassphraseBytes
			|| Passphrase.Num() > MaxRecoveryPassphraseBytes)
		{
			return Fail(
				OutError,
				FString::Printf(
					TEXT("恢复密码必须是 %d 至 %d 字节的 UTF-8 文本。"),
					MinRecoveryPassphraseBytes,
					MaxRecoveryPassphraseBytes));
		}
		return true;
	}

	FString RecoveryAdditionalData(const FRecoveryBundleFields& Fields)
	{
		FString Result = FString::Printf(
			TEXT("AkUGCPlatform.PublisherRecovery.v1\n")
			TEXT("kind=%s\n")
			TEXT("protection=%s\n")
			TEXT("publicKey=%s\n")
			TEXT("keyVersion=%d\n")
			TEXT("kdf=%s\n")
			TEXT("kdfN=%llu\n")
			TEXT("kdfR=%llu\n")
			TEXT("kdfP=%llu\n")
			TEXT("salt=%s\n")
			TEXT("cipher=%s\n")
			TEXT("nonce=%s\n")
			TEXT("rotationCount=%d"),
			RecoveryKind,
			RecoveryProtectionName,
			*Fields.Info.PublicKey,
			Fields.Info.KeyVersion,
			RecoveryKdfName,
			static_cast<unsigned long long>(RecoveryScryptN),
			static_cast<unsigned long long>(RecoveryScryptR),
			static_cast<unsigned long long>(RecoveryScryptP),
			*FBase64::Encode(Fields.Salt.GetData(), Fields.Salt.Num()),
			RecoveryCipherName,
			*FBase64::Encode(Fields.Nonce.GetData(), Fields.Nonce.Num()),
			Fields.Info.Rotations.Num());
		for (const FAkUGCPublisherKeyRotation& Rotation : Fields.Info.Rotations)
		{
			Result += TEXT("\n") + RotationMessage(Rotation);
			Result += TEXT("\npreviousSignature=") + Rotation.PreviousSignature;
			Result += TEXT("\nnewSignature=") + Rotation.NewSignature;
		}
		return Result;
	}

	bool DeriveRecoveryKey(
		TConstArrayView<uint8> Passphrase,
		TConstArrayView<uint8> Salt,
		uint8 (&OutKey)[RecoveryKeyBytes],
		FString* OutError)
	{
		FMemory::Memzero(OutKey, sizeof(OutKey));
		if (!ValidateRecoveryPassphrase(Passphrase, OutError)
			|| Salt.Num() != RecoverySaltBytes)
		{
			return false;
		}
		if (EVP_PBE_scrypt(
				reinterpret_cast<const char*>(Passphrase.GetData()),
				static_cast<size_t>(Passphrase.Num()),
				Salt.GetData(),
				static_cast<size_t>(Salt.Num()),
				RecoveryScryptN,
				RecoveryScryptR,
				RecoveryScryptP,
				RecoveryScryptMaxMemory,
				OutKey,
				sizeof(OutKey)) != 1)
		{
			FMemory::Memzero(OutKey, sizeof(OutKey));
			return Fail(OutError, TEXT("无法派生恢复包加密密钥。"));
		}
		return true;
	}

	bool EncryptRecoveryPrivateKey(
		TConstArrayView<uint8> PrivateKey,
		TConstArrayView<uint8> Passphrase,
		FRecoveryBundleFields& InOutFields,
		FString* OutError)
	{
		if (PrivateKey.Num() != Ed25519PrivateKeyBytes
			|| InOutFields.Salt.Num() != RecoverySaltBytes
			|| InOutFields.Nonce.Num() != RecoveryNonceBytes)
		{
			return Fail(OutError, TEXT("恢复包加密输入长度非法。"));
		}

		uint8 Key[RecoveryKeyBytes] = {};
		if (!DeriveRecoveryKey(Passphrase, InOutFields.Salt, Key, OutError))
		{
			return false;
		}

		const FTCHARToUTF8 AdditionalDataUtf8(
			*RecoveryAdditionalData(InOutFields));
		EVP_CIPHER_CTX* Context = EVP_CIPHER_CTX_new();
		InOutFields.Ciphertext.SetNumUninitialized(
			PrivateKey.Num() + EVP_MAX_BLOCK_LENGTH);
		InOutFields.Tag.SetNumUninitialized(RecoveryTagBytes);
		int32 OutputLength = 0;
		int32 FinalLength = 0;
		const bool bEncrypted = Context
			&& EVP_EncryptInit_ex(
				Context,
				EVP_aes_256_gcm(),
				nullptr,
				nullptr,
				nullptr) == 1
			&& EVP_CIPHER_CTX_ctrl(
				Context,
				EVP_CTRL_GCM_SET_IVLEN,
				InOutFields.Nonce.Num(),
				nullptr) == 1
			&& EVP_EncryptInit_ex(
				Context,
				nullptr,
				nullptr,
				Key,
				InOutFields.Nonce.GetData()) == 1
			&& EVP_EncryptUpdate(
				Context,
				nullptr,
				&OutputLength,
				reinterpret_cast<const uint8*>(AdditionalDataUtf8.Get()),
				AdditionalDataUtf8.Length()) == 1
			&& EVP_EncryptUpdate(
				Context,
				InOutFields.Ciphertext.GetData(),
				&OutputLength,
				PrivateKey.GetData(),
				PrivateKey.Num()) == 1
			&& OutputLength == PrivateKey.Num()
			&& EVP_EncryptFinal_ex(
				Context,
				InOutFields.Ciphertext.GetData() + OutputLength,
				&FinalLength) == 1
			&& OutputLength + FinalLength == PrivateKey.Num()
			&& EVP_CIPHER_CTX_ctrl(
				Context,
				EVP_CTRL_GCM_GET_TAG,
				InOutFields.Tag.Num(),
				InOutFields.Tag.GetData()) == 1;
		if (Context)
		{
			EVP_CIPHER_CTX_free(Context);
		}
		FMemory::Memzero(Key, sizeof(Key));
		if (!bEncrypted)
		{
			SecureReset(InOutFields.Ciphertext);
			SecureReset(InOutFields.Tag);
			return Fail(OutError, TEXT("无法加密发布身份恢复包。"));
		}
		InOutFields.Ciphertext.SetNum(OutputLength + FinalLength, EAllowShrinking::No);
		return true;
	}

	bool DecryptRecoveryPrivateKey(
		const FRecoveryBundleFields& Fields,
		TConstArrayView<uint8> Passphrase,
		TArray<uint8>& OutPrivateKey,
		FString* OutError)
	{
		SecureReset(OutPrivateKey);
		if (Fields.Salt.Num() != RecoverySaltBytes
			|| Fields.Nonce.Num() != RecoveryNonceBytes
			|| Fields.Ciphertext.Num() != Ed25519PrivateKeyBytes
			|| Fields.Tag.Num() != RecoveryTagBytes)
		{
			return Fail(OutError, TEXT("恢复包密文字段长度非法。"));
		}

		uint8 Key[RecoveryKeyBytes] = {};
		if (!DeriveRecoveryKey(Passphrase, Fields.Salt, Key, OutError))
		{
			return false;
		}

		const FTCHARToUTF8 AdditionalDataUtf8(*RecoveryAdditionalData(Fields));
		EVP_CIPHER_CTX* Context = EVP_CIPHER_CTX_new();
		OutPrivateKey.SetNumUninitialized(
			Ed25519PrivateKeyBytes + EVP_MAX_BLOCK_LENGTH);
		int32 OutputLength = 0;
		int32 FinalLength = 0;
		const bool bDecrypted = Context
			&& EVP_DecryptInit_ex(
				Context,
				EVP_aes_256_gcm(),
				nullptr,
				nullptr,
				nullptr) == 1
			&& EVP_CIPHER_CTX_ctrl(
				Context,
				EVP_CTRL_GCM_SET_IVLEN,
				Fields.Nonce.Num(),
				nullptr) == 1
			&& EVP_DecryptInit_ex(
				Context,
				nullptr,
				nullptr,
				Key,
				Fields.Nonce.GetData()) == 1
			&& EVP_DecryptUpdate(
				Context,
				nullptr,
				&OutputLength,
				reinterpret_cast<const uint8*>(AdditionalDataUtf8.Get()),
				AdditionalDataUtf8.Length()) == 1
			&& EVP_DecryptUpdate(
				Context,
				OutPrivateKey.GetData(),
				&OutputLength,
				Fields.Ciphertext.GetData(),
				Fields.Ciphertext.Num()) == 1
			&& OutputLength == Ed25519PrivateKeyBytes
			&& EVP_CIPHER_CTX_ctrl(
				Context,
				EVP_CTRL_GCM_SET_TAG,
				Fields.Tag.Num(),
				const_cast<uint8*>(Fields.Tag.GetData())) == 1
			&& EVP_DecryptFinal_ex(
				Context,
				OutPrivateKey.GetData() + OutputLength,
				&FinalLength) == 1
			&& OutputLength + FinalLength == Ed25519PrivateKeyBytes;
		if (Context)
		{
			EVP_CIPHER_CTX_free(Context);
		}
		FMemory::Memzero(Key, sizeof(Key));
		if (!bDecrypted)
		{
			SecureReset(OutPrivateKey);
			return Fail(
				OutError,
				TEXT("恢复包密码错误或内容已被篡改；当前身份未改变。"));
		}
		OutPrivateKey.SetNum(
			OutputLength + FinalLength,
			EAllowShrinking::No);
		return true;
	}

	bool SerializeRecoveryBundle(
		const FRecoveryBundleFields& Fields,
		FString& OutJson,
		FString* OutError)
	{
		if (!VerifyIdentityHistoryInternal(Fields.Info, OutError)
			|| Fields.Salt.Num() != RecoverySaltBytes
			|| Fields.Nonce.Num() != RecoveryNonceBytes
			|| Fields.Ciphertext.Num() != Ed25519PrivateKeyBytes
			|| Fields.Tag.Num() != RecoveryTagBytes)
		{
			return Fail(OutError, TEXT("恢复包字段不完整，无法序列化。"));
		}
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetNumberField(TEXT("version"), RecoveryBundleVersion);
		Object->SetStringField(TEXT("kind"), RecoveryKind);
		Object->SetStringField(TEXT("protection"), RecoveryProtectionName);
		Object->SetStringField(TEXT("publicKey"), Fields.Info.PublicKey);
		Object->SetNumberField(TEXT("keyVersion"), Fields.Info.KeyVersion);
		Object->SetArrayField(
			TEXT("rotations"),
			SerializeRotations(Fields.Info.Rotations));
		Object->SetStringField(TEXT("kdf"), RecoveryKdfName);
		Object->SetNumberField(
			TEXT("kdfN"),
			static_cast<double>(RecoveryScryptN));
		Object->SetNumberField(
			TEXT("kdfR"),
			static_cast<double>(RecoveryScryptR));
		Object->SetNumberField(
			TEXT("kdfP"),
			static_cast<double>(RecoveryScryptP));
		Object->SetStringField(
			TEXT("salt"),
			FBase64::Encode(Fields.Salt.GetData(), Fields.Salt.Num()));
		Object->SetStringField(TEXT("cipher"), RecoveryCipherName);
		Object->SetStringField(
			TEXT("nonce"),
			FBase64::Encode(Fields.Nonce.GetData(), Fields.Nonce.Num()));
		Object->SetStringField(
			TEXT("ciphertext"),
			FBase64::Encode(
				Fields.Ciphertext.GetData(),
				Fields.Ciphertext.Num()));
		Object->SetStringField(
			TEXT("tag"),
			FBase64::Encode(Fields.Tag.GetData(), Fields.Tag.Num()));

		OutJson.Reset();
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(
				&OutJson);
		if (!FJsonSerializer::Serialize(Object, Writer)
			|| FTCHARToUTF8(*OutJson).Length() > MaxRecoveryBundleBytes)
		{
			OutJson.Reset();
			return Fail(OutError, TEXT("恢复包序列化失败或超过大小限制。"));
		}
		return true;
	}

	bool ParseRecoveryBundle(
		const FString& Json,
		FRecoveryBundleFields& OutFields,
		FString* OutError)
	{
		OutFields = FRecoveryBundleFields();
		if (!AkUGCInputBudget::ValidateJson(
				Json,
				MaxRecoveryBundleBytes,
				TEXT("Publisher recovery bundle"),
				OutError))
		{
			return false;
		}
		TSharedPtr<FJsonObject> Object;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Object)
			|| !Object.IsValid()
			|| Object->Values.Num() != 15)
		{
			return Fail(OutError, TEXT("恢复包 JSON 格式错误。"));
		}

		int32 Version = 0;
		int64 KdfN = 0;
		int64 KdfR = 0;
		int64 KdfP = 0;
		FString Kind;
		FString Protection;
		FString Kdf;
		FString Cipher;
		FString Salt;
		FString Nonce;
		FString Ciphertext;
		FString Tag;
		if (!TryGetExactInt32(Object, TEXT("version"), Version)
			|| Version != RecoveryBundleVersion
			|| !Object->TryGetStringField(TEXT("kind"), Kind)
			|| Kind != RecoveryKind
			|| !Object->TryGetStringField(TEXT("protection"), Protection)
			|| Protection != RecoveryProtectionName
			|| !Object->TryGetStringField(
				TEXT("publicKey"),
				OutFields.Info.PublicKey)
			|| !TryGetExactInt32(
				Object,
				TEXT("keyVersion"),
				OutFields.Info.KeyVersion)
			|| !ParseRotations(Object, OutFields.Info.Rotations, OutError)
			|| !Object->TryGetStringField(TEXT("kdf"), Kdf)
			|| Kdf != RecoveryKdfName
			|| !TryGetExactInt64(Object, TEXT("kdfN"), KdfN)
			|| KdfN != static_cast<int64>(RecoveryScryptN)
			|| !TryGetExactInt64(Object, TEXT("kdfR"), KdfR)
			|| KdfR != static_cast<int64>(RecoveryScryptR)
			|| !TryGetExactInt64(Object, TEXT("kdfP"), KdfP)
			|| KdfP != static_cast<int64>(RecoveryScryptP)
			|| !Object->TryGetStringField(TEXT("salt"), Salt)
			|| !Object->TryGetStringField(TEXT("cipher"), Cipher)
			|| Cipher != RecoveryCipherName
			|| !Object->TryGetStringField(TEXT("nonce"), Nonce)
			|| !Object->TryGetStringField(TEXT("ciphertext"), Ciphertext)
			|| !Object->TryGetStringField(TEXT("tag"), Tag)
			|| !DecodeCanonicalBase64(
				Salt,
				RecoverySaltBytes,
				OutFields.Salt)
			|| !DecodeCanonicalBase64(
				Nonce,
				RecoveryNonceBytes,
				OutFields.Nonce)
			|| !DecodeCanonicalBase64(
				Ciphertext,
				Ed25519PrivateKeyBytes,
				OutFields.Ciphertext)
			|| !DecodeCanonicalBase64(Tag, RecoveryTagBytes, OutFields.Tag)
			|| !VerifyIdentityHistoryInternal(OutFields.Info, OutError))
		{
			OutFields = FRecoveryBundleFields();
			return Fail(OutError, TEXT("恢复包字段错误、版本不兼容或历史链无效。"));
		}
		return true;
	}

	bool BuildRecoveryBundle(
		TConstArrayView<uint8> PrivateKey,
		const FAkUGCPublisherIdentityInfo& Info,
		TConstArrayView<uint8> Passphrase,
		FString& OutJson,
		FString* OutError)
	{
		OutJson.Reset();
		FRecoveryBundleFields Fields;
		Fields.Info = Info;
		Fields.Salt.SetNumUninitialized(RecoverySaltBytes);
		Fields.Nonce.SetNumUninitialized(RecoveryNonceBytes);
		if (RAND_bytes(Fields.Salt.GetData(), Fields.Salt.Num()) != 1
			|| RAND_bytes(Fields.Nonce.GetData(), Fields.Nonce.Num()) != 1)
		{
			return Fail(OutError, TEXT("无法生成恢复包随机盐或随机数。"));
		}
		return EncryptRecoveryPrivateKey(
				PrivateKey,
				Passphrase,
				Fields,
				OutError)
			&& SerializeRecoveryBundle(Fields, OutJson, OutError);
	}

	bool ReadRecoveryBundle(
		const FString& Path,
		TConstArrayView<uint8> Passphrase,
		TArray<uint8>& OutPrivateKey,
		FAkUGCPublisherIdentityInfo& OutInfo,
		FString* OutError)
	{
		SecureReset(OutPrivateKey);
		OutInfo = FAkUGCPublisherIdentityInfo();
		FString Json;
		if (!ReadBoundedText(
				Path,
				MaxRecoveryBundleBytes,
				Json,
				OutError,
				TEXT("发布身份恢复包")))
		{
			return false;
		}
		FRecoveryBundleFields Fields;
		if (!ParseRecoveryBundle(Json, Fields, OutError)
			|| !DecryptRecoveryPrivateKey(
				Fields,
				Passphrase,
				OutPrivateKey,
				OutError))
		{
			return false;
		}
		FString DerivedPublicKey;
		if (!DerivePublicKeyRaw(
				OutPrivateKey,
				DerivedPublicKey,
				OutError)
			|| DerivedPublicKey != Fields.Info.PublicKey)
		{
			SecureReset(OutPrivateKey);
			return Fail(
				OutError,
				TEXT("恢复包中的私钥与当前公钥不匹配。"));
		}
		OutInfo = MoveTemp(Fields.Info);
		return true;
	}

	bool WriteNewRecoveryBundle(
		const FString& Path,
		TConstArrayView<uint8> PrivateKey,
		const FAkUGCPublisherIdentityInfo& Info,
		TConstArrayView<uint8> Passphrase,
		FString* OutError)
	{
		if (Path.IsEmpty()
			|| !FPaths::GetExtension(Path).Equals(
				TEXT("akugcrecovery"),
				ESearchCase::IgnoreCase)
			|| IFileManager::Get().FileExists(*Path)
			|| IFileManager::Get().DirectoryExists(*Path))
		{
			return Fail(
				OutError,
				TEXT("恢复包路径必须是尚不存在的 .akugcrecovery 文件。"));
		}
		TArray<FString> StaleTemporaryFiles;
		IFileManager::Get().FindFiles(
			StaleTemporaryFiles,
			*(Path + TEXT(".*.tmp")),
			true,
			false);
		if (!StaleTemporaryFiles.IsEmpty())
		{
			StaleTemporaryFiles.Sort();
			return Fail(
				OutError,
				TEXT("检测到未完成的恢复包临时文件；请先隔离并人工处理：")
					+ FString::Join(StaleTemporaryFiles, TEXT(", ")));
		}
		if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true))
		{
			return Fail(OutError, TEXT("无法创建恢复包目录。"));
		}

		FString Json;
		if (!BuildRecoveryBundle(
				PrivateKey,
				Info,
				Passphrase,
				Json,
				OutError))
		{
			return false;
		}
		const FString Temporary =
			Path + TEXT(".") + FGuid::NewGuid().ToString(EGuidFormats::Digits)
			+ TEXT(".tmp");
		if (!FFileHelper::SaveStringToFile(
				Json,
				*Temporary,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			IFileManager::Get().Delete(*Temporary, false, false, true);
			return Fail(OutError, TEXT("恢复包临时文件写入失败。"));
		}

		TArray<uint8> VerifiedPrivateKey;
		FAkUGCPublisherIdentityInfo VerifiedInfo;
		FString VerificationError;
		const bool bVerified =
			ReadRecoveryBundle(
				Temporary,
				Passphrase,
				VerifiedPrivateKey,
				VerifiedInfo,
				&VerificationError)
			&& PrivateKeysEqual(VerifiedPrivateKey, PrivateKey)
			&& IdentityInfoEqual(VerifiedInfo, Info);
		SecureReset(VerifiedPrivateKey);
		if (!bVerified)
		{
			IFileManager::Get().Delete(*Temporary, false, false, true);
			return Fail(
				OutError,
				TEXT("恢复包临时文件回读校验失败：") + VerificationError);
		}
		if (!IFileManager::Get().Move(
				*Path,
				*Temporary,
				false,
				false,
				false,
				true))
		{
			IFileManager::Get().Delete(*Temporary, false, false, true);
			return Fail(OutError, TEXT("恢复包原子写入失败。"));
		}
		return true;
	}

	bool NormalizeRecoveryPath(
		const FString& Path,
		const FString& PublisherRoot,
		FString& OutPath,
		FString* OutError)
	{
		OutPath = Path.IsEmpty()
			? FString()
			: FPaths::ConvertRelativePathToFull(Path);
		FString FullRoot = FPaths::ConvertRelativePathToFull(PublisherRoot);
		FPaths::NormalizeFilename(OutPath);
		FPaths::NormalizeDirectoryName(FullRoot);
		if (OutPath.IsEmpty()
			|| OutPath.Equals(FullRoot, ESearchCase::IgnoreCase)
			|| OutPath.StartsWith(
				FullRoot + TEXT("/"),
				ESearchCase::IgnoreCase))
		{
			OutPath.Reset();
			return Fail(
				OutError,
				TEXT("恢复包不得放在发布身份凭据目录内。"));
		}
		return true;
	}
}

FAkUGCPublisherKeyStore::FAkUGCPublisherKeyStore(const FString& Directory)
	: Root(Directory.IsEmpty() ? FString() : FPaths::ConvertRelativePathToFull(Directory))
{
}

FString FAkUGCPublisherKeyStore::DefaultDirectory()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UGC/Publisher"));
}

FString FAkUGCPublisherKeyStore::IdentityPath() const
{
	using namespace AkUGCPublisherKeyStorePrivate;
	return Root.IsEmpty() ? FString() : FPaths::Combine(Root, IdentityFileName);
}

FString FAkUGCPublisherKeyStore::LegacyPrivateKeyPath() const
{
	using namespace AkUGCPublisherKeyStorePrivate;
	return Root.IsEmpty() ? FString() : FPaths::Combine(Root, LegacyPrivateKeyFileName);
}

bool FAkUGCPublisherKeyStore::LoadOrCreate(
	TArray<uint8>& OutPrivateKey,
	FString& OutPublicKey,
	FAkUGCPublisherIdentityInfo* OutInfo,
	FString* OutError) const
{
	using namespace AkUGCPublisherKeyStorePrivate;
	using AkUGCPublisherKeyStorePrivate::Fail;
	using AkUGCPublisherKeyStorePrivate::SecureReset;
	SecureReset(OutPrivateKey);
	OutPublicKey.Reset();
	if (OutInfo)
	{
		*OutInfo = FAkUGCPublisherIdentityInfo();
	}
	if (OutError)
	{
		OutError->Reset();
	}
	if (Root.IsEmpty())
	{
		return Fail(OutError, TEXT("发布身份目录不能为空。"));
	}
#if !PLATFORM_WINDOWS
	return Fail(OutError, TEXT("当前平台不支持发布身份；P18 仅启用 Win64 DPAPI。"));
#else
	FPublisherIdentityFileLock StoreLock;
	if (!StoreLock.Acquire(Root, OutError))
	{
		return false;
	}
	const bool bLoaded = LoadOrCreateLocked(
		OutPrivateKey,
		OutPublicKey,
		OutInfo,
		OutError);
	if (!bLoaded)
	{
		SecureReset(OutPrivateKey);
		OutPublicKey.Reset();
		if (OutInfo)
		{
			*OutInfo = FAkUGCPublisherIdentityInfo();
		}
	}
	return bLoaded;
#endif
}

bool FAkUGCPublisherKeyStore::LoadOrCreateLocked(
	TArray<uint8>& OutPrivateKey,
	FString& OutPublicKey,
	FAkUGCPublisherIdentityInfo* OutInfo,
	FString* OutError) const
{
	using namespace AkUGCPublisherKeyStorePrivate;
	using AkUGCPublisherKeyStorePrivate::Fail;
	using AkUGCPublisherKeyStorePrivate::SecureReset;
	SecureReset(OutPrivateKey);
	OutPublicKey.Reset();
	if (OutInfo)
	{
		*OutInfo = FAkUGCPublisherIdentityInfo();
	}
	const FString ProtectedPath = IdentityPath();
	const FString LegacyPath = LegacyPrivateKeyPath();
	if (IFileManager::Get().DirectoryExists(*ProtectedPath)
		|| IFileManager::Get().DirectoryExists(*LegacyPath))
	{
		return Fail(OutError, TEXT("发布身份文件路径被目录占用。"));
	}
	if (!RejectSensitiveTemporaryFiles(Root, OutError))
	{
		return false;
	}

	auto ValidateLoadedIdentity = [OutError](
		TArray<uint8>& PrivateKey,
		const FString& DeclaredPublicKey) -> bool
	{
		FString DerivedPublicKey;
		if (!FAkUGCLogicPackSigner::DerivePublicKey(
				PrivateKey,
				DerivedPublicKey,
				OutError)
			|| DerivedPublicKey != DeclaredPublicKey)
		{
			SecureReset(PrivateKey);
			return Fail(OutError, TEXT("受保护发布身份的公私钥不匹配，禁止自动替换。"));
		}
		return true;
	};

	auto LoadProtected = [&]() -> bool
	{
		FAkUGCPublisherIdentityInfo LoadedInfo;
		if (!LoadIdentityFile(
				ProtectedPath,
				OutPrivateKey,
				LoadedInfo,
				OutError))
		{
			SecureReset(OutPrivateKey);
			OutPublicKey.Reset();
			if (OutInfo)
			{
				*OutInfo = FAkUGCPublisherIdentityInfo();
			}
			return false;
		}
		OutPublicKey = LoadedInfo.PublicKey;
		if (!ValidateLoadedIdentity(OutPrivateKey, OutPublicKey))
		{
			OutPublicKey.Reset();
			if (OutInfo)
			{
				*OutInfo = FAkUGCPublisherIdentityInfo();
			}
			return false;
		}
		if (OutInfo)
		{
			*OutInfo = MoveTemp(LoadedInfo);
		}
		return true;
	};

	auto EnsureMigrationFence = [&]() -> bool
	{
		for (int32 Attempt = 0; Attempt < 3; ++Attempt)
		{
			ELegacyIdentityState LegacyState = ELegacyIdentityState::Missing;
			TArray<uint8> LegacyPrivateKey;
			if (!InspectLegacyIdentity(
					LegacyPath,
					LegacyState,
					LegacyPrivateKey,
					OutError))
			{
				SecureReset(LegacyPrivateKey);
				return false;
			}
			if (LegacyState == ELegacyIdentityState::MigrationFence)
			{
				return true;
			}
			if (LegacyState == ELegacyIdentityState::Plaintext)
			{
				const bool bMatches =
					PrivateKeysEqual(OutPrivateKey, LegacyPrivateKey);
				SecureReset(LegacyPrivateKey);
				if (!bMatches)
				{
					return Fail(
						OutError,
						TEXT("检测到与受保护身份冲突的旧版明文私钥；禁止覆盖或自动换钥。"));
				}
				return WriteMigrationFence(LegacyPath, true, OutError);
			}
			FString FenceError;
			if (WriteMigrationFence(LegacyPath, false, &FenceError))
			{
				return true;
			}
			if (!IFileManager::Get().FileExists(*LegacyPath))
			{
				return Fail(OutError, FenceError);
			}
		}
		return Fail(OutError, TEXT("无法稳定写入旧版发布程序兼容阻断标记。"));
	};

	if (IFileManager::Get().FileExists(*ProtectedPath))
	{
		if (!LoadProtected() || !EnsureMigrationFence())
		{
			SecureReset(OutPrivateKey);
			OutPublicKey.Reset();
			return false;
		}
		return true;
	}

	for (int32 Attempt = 0; Attempt < 3; ++Attempt)
	{
		ELegacyIdentityState LegacyState = ELegacyIdentityState::Missing;
		TArray<uint8> LegacyPrivateKey;
		if (!InspectLegacyIdentity(
				LegacyPath,
				LegacyState,
				LegacyPrivateKey,
				OutError))
		{
			SecureReset(LegacyPrivateKey);
			return false;
		}

		if (LegacyState == ELegacyIdentityState::Plaintext)
		{
			FString LegacyPublicKey;
			if (!FAkUGCLogicPackSigner::DerivePublicKey(
					LegacyPrivateKey,
					LegacyPublicKey,
					OutError))
			{
				SecureReset(LegacyPrivateKey);
				return false;
			}
			FAkUGCPublisherIdentityInfo LegacyInfo;
			LegacyInfo.KeyVersion = 1;
			LegacyInfo.PublicKey = LegacyPublicKey;
			FString WriteError;
			if (!WriteIdentityFile(
					ProtectedPath,
					LegacyPrivateKey,
					LegacyInfo,
					false,
					&WriteError)
				&& !IFileManager::Get().FileExists(*ProtectedPath))
			{
				SecureReset(LegacyPrivateKey);
				return Fail(OutError, WriteError);
			}
			if (!LoadProtected()
				|| OutPublicKey != LegacyPublicKey
				|| !PrivateKeysEqual(OutPrivateKey, LegacyPrivateKey))
			{
				SecureReset(LegacyPrivateKey);
				SecureReset(OutPrivateKey);
				OutPublicKey.Reset();
				return Fail(
					OutError,
					TEXT("旧版发布私钥迁移后的身份校验失败；原明文文件保持不变。"));
			}
			SecureReset(LegacyPrivateKey);
			if (!WriteMigrationFence(LegacyPath, true, OutError))
			{
				SecureReset(OutPrivateKey);
				OutPublicKey.Reset();
				return false;
			}
			return true;
		}
		SecureReset(LegacyPrivateKey);

		if (LegacyState == ELegacyIdentityState::Missing)
		{
			FString FenceError;
			if (!WriteMigrationFence(LegacyPath, false, &FenceError))
			{
				if (IFileManager::Get().FileExists(*LegacyPath))
				{
					continue;
				}
				return Fail(OutError, FenceError);
			}
		}

		if (IFileManager::Get().FileExists(*ProtectedPath))
		{
			if (!LoadProtected() || !EnsureMigrationFence())
			{
				SecureReset(OutPrivateKey);
				OutPublicKey.Reset();
				return false;
			}
			return true;
		}

		FAkUGCLogicPackKeyPair Generated;
		if (!FAkUGCLogicPackSigner::GenerateKeyPair(Generated, OutError))
		{
			return false;
		}
		TArray<uint8> GeneratedPrivateKey;
		const bool bPrivateKeyValid =
			HexToPrivateKeyBytes(Generated.PrivateKey, GeneratedPrivateKey);
		SecureReset(Generated.PrivateKey);
		if (!bPrivateKeyValid)
		{
			return Fail(OutError, TEXT("新生成的发布私钥格式非法。"));
		}

		FString DerivedPublicKey;
		if (!FAkUGCLogicPackSigner::DerivePublicKey(
				GeneratedPrivateKey,
				DerivedPublicKey,
				OutError)
			|| DerivedPublicKey != Generated.PublicKey)
		{
			SecureReset(GeneratedPrivateKey);
			return Fail(OutError, TEXT("新生成的发布身份公私钥不匹配。"));
		}

		FString WriteError;
		FAkUGCPublisherIdentityInfo GeneratedInfo;
		GeneratedInfo.KeyVersion = 1;
		GeneratedInfo.PublicKey = Generated.PublicKey;
		const bool bWritten = WriteIdentityFile(
			ProtectedPath,
			GeneratedPrivateKey,
			GeneratedInfo,
			false,
			&WriteError);
		SecureReset(GeneratedPrivateKey);
		if (!bWritten && !IFileManager::Get().FileExists(*ProtectedPath))
		{
			return Fail(OutError, WriteError);
		}
		if (!LoadProtected() || !EnsureMigrationFence())
		{
			SecureReset(OutPrivateKey);
			OutPublicKey.Reset();
			return false;
		}
		return true;
	}

	return Fail(OutError, TEXT("发布身份状态持续变化，无法安全创建或迁移。"));
}

bool FAkUGCPublisherKeyStore::GetOrCreatePublicKey(
	FString& OutPublicKey,
	FString* OutError) const
{
	using namespace AkUGCPublisherKeyStorePrivate;
	using AkUGCPublisherKeyStorePrivate::SecureReset;
	TArray<uint8> PrivateKey;
	const bool bLoaded = LoadOrCreate(
		PrivateKey,
		OutPublicKey,
		nullptr,
		OutError);
	SecureReset(PrivateKey);
	return bLoaded;
}

bool FAkUGCPublisherKeyStore::GetIdentityInfo(
	FAkUGCPublisherIdentityInfo& OutInfo,
	FString* OutError) const
{
	using namespace AkUGCPublisherKeyStorePrivate;
	using AkUGCPublisherKeyStorePrivate::SecureReset;
	TArray<uint8> PrivateKey;
	FString PublicKey;
	const bool bLoaded = LoadOrCreate(
		PrivateKey,
		PublicKey,
		&OutInfo,
		OutError);
	SecureReset(PrivateKey);
	return bLoaded;
}

bool FAkUGCPublisherKeyStore::Sign(
	const FAkUGCLogicPackManifest& Manifest,
	FAkUGCLogicPackSignature& OutSignature,
	FString* OutError) const
{
	using namespace AkUGCPublisherKeyStorePrivate;
	using AkUGCPublisherKeyStorePrivate::Fail;
	using AkUGCPublisherKeyStorePrivate::SecureReset;
	TArray<uint8> PrivateKey;
	FString PublicKey;
	if (!LoadOrCreate(PrivateKey, PublicKey, nullptr, OutError))
	{
		SecureReset(PrivateKey);
		return false;
	}
	const bool bSigned = FAkUGCLogicPackSigner::SignPrivateKeyBytes(
		Manifest,
		PrivateKey,
		OutSignature,
		OutError);
	SecureReset(PrivateKey);
	if (!bSigned)
	{
		return false;
	}
	if (OutSignature.PublicKey != PublicKey)
	{
		OutSignature = FAkUGCLogicPackSignature();
		return Fail(OutError, TEXT("发布身份签名公钥与存储身份不一致。"));
	}
	return true;
}

bool FAkUGCPublisherKeyStore::ExportRecoveryBundle(
	const FString& OutputPath,
	TConstArrayView<uint8> PassphraseUtf8,
	FString* OutError) const
{
	using namespace AkUGCPublisherKeyStorePrivate;
	using AkUGCPublisherKeyStorePrivate::Fail;
	using AkUGCPublisherKeyStorePrivate::SecureReset;
	if (OutError)
	{
		OutError->Reset();
	}
	if (Root.IsEmpty())
	{
		return Fail(OutError, TEXT("发布身份目录不能为空。"));
	}
	FString FullOutputPath;
	if (!NormalizeRecoveryPath(
			OutputPath,
			Root,
			FullOutputPath,
			OutError)
		|| !ValidateRecoveryPassphrase(PassphraseUtf8, OutError))
	{
		return false;
	}
#if !PLATFORM_WINDOWS
	return Fail(OutError, TEXT("当前平台不支持导出发布身份恢复包。"));
#else
	FPublisherIdentityFileLock StoreLock;
	if (!StoreLock.Acquire(Root, OutError))
	{
		return false;
	}
	TArray<uint8> PrivateKey;
	FString PublicKey;
	FAkUGCPublisherIdentityInfo Info;
	if (!LoadOrCreateLocked(PrivateKey, PublicKey, &Info, OutError))
	{
		SecureReset(PrivateKey);
		return false;
	}
	const bool bWritten = WriteNewRecoveryBundle(
		FullOutputPath,
		PrivateKey,
		Info,
		PassphraseUtf8,
		OutError);
	SecureReset(PrivateKey);
	return bWritten;
#endif
}

bool FAkUGCPublisherKeyStore::ImportRecoveryBundle(
	const FString& InputPath,
	TConstArrayView<uint8> PassphraseUtf8,
	FAkUGCPublisherIdentityInfo& OutInfo,
	FString* OutError) const
{
	using namespace AkUGCPublisherKeyStorePrivate;
	using AkUGCPublisherKeyStorePrivate::Fail;
	using AkUGCPublisherKeyStorePrivate::SecureReset;
	OutInfo = FAkUGCPublisherIdentityInfo();
	if (OutError)
	{
		OutError->Reset();
	}
	if (Root.IsEmpty())
	{
		return Fail(OutError, TEXT("发布身份目录不能为空。"));
	}
	FString FullInputPath;
	if (!NormalizeRecoveryPath(
			InputPath,
			Root,
			FullInputPath,
			OutError)
		|| !FPaths::GetExtension(FullInputPath).Equals(
			TEXT("akugcrecovery"),
			ESearchCase::IgnoreCase)
		|| !ValidateRecoveryPassphrase(PassphraseUtf8, OutError))
	{
		return Fail(
			OutError,
			OutError && !OutError->IsEmpty()
				? *OutError
				: TEXT("恢复包必须使用 .akugcrecovery 扩展名。"));
	}

	TArray<uint8> RecoveredPrivateKey;
	FAkUGCPublisherIdentityInfo RecoveredInfo;
	if (!ReadRecoveryBundle(
			FullInputPath,
			PassphraseUtf8,
			RecoveredPrivateKey,
			RecoveredInfo,
			OutError))
	{
		SecureReset(RecoveredPrivateKey);
		return false;
	}

#if !PLATFORM_WINDOWS
	SecureReset(RecoveredPrivateKey);
	return Fail(OutError, TEXT("当前平台不支持导入发布身份恢复包。"));
#else
	FPublisherIdentityFileLock StoreLock;
	if (!StoreLock.Acquire(Root, OutError))
	{
		SecureReset(RecoveredPrivateKey);
		return false;
	}
	const FString ProtectedPath = IdentityPath();
	const FString LegacyPath = LegacyPrivateKeyPath();
	if (IFileManager::Get().DirectoryExists(*ProtectedPath)
		|| IFileManager::Get().DirectoryExists(*LegacyPath)
		|| !RejectSensitiveTemporaryFiles(Root, OutError))
	{
		SecureReset(RecoveredPrivateKey);
		return false;
	}

	ELegacyIdentityState LegacyState = ELegacyIdentityState::Missing;
	TArray<uint8> LegacyPrivateKey;
	if (!InspectLegacyIdentity(
			LegacyPath,
			LegacyState,
			LegacyPrivateKey,
			OutError))
	{
		SecureReset(LegacyPrivateKey);
		SecureReset(RecoveredPrivateKey);
		return false;
	}
	if (LegacyState == ELegacyIdentityState::Plaintext
		&& !PrivateKeysEqual(LegacyPrivateKey, RecoveredPrivateKey))
	{
		SecureReset(LegacyPrivateKey);
		SecureReset(RecoveredPrivateKey);
		return Fail(
			OutError,
			TEXT("恢复包与现有旧版明文身份冲突；禁止覆盖。"));
	}
	SecureReset(LegacyPrivateKey);

	if (IFileManager::Get().FileExists(*ProtectedPath))
	{
		TArray<uint8> ExistingPrivateKey;
		FAkUGCPublisherIdentityInfo ExistingInfo;
		FString ExistingLoadError;
		const bool bLoaded = LoadIdentityFile(
			ProtectedPath,
			ExistingPrivateKey,
			ExistingInfo,
			&ExistingLoadError);
		const bool bSame = bLoaded
			&& PrivateKeysEqual(ExistingPrivateKey, RecoveredPrivateKey)
			&& IdentityInfoEqual(ExistingInfo, RecoveredInfo);
		SecureReset(ExistingPrivateKey);
		if (!bSame)
		{
			FAkUGCPublisherIdentityInfo DeclaredInfo;
			FString MetadataError;
			if (!LoadIdentityMetadata(
					ProtectedPath,
					DeclaredInfo,
					&MetadataError)
				|| !IdentityInfoEqual(DeclaredInfo, RecoveredInfo))
			{
				SecureReset(RecoveredPrivateKey);
				return Fail(
					OutError,
					TEXT("恢复包与现有发布身份或轮换历史冲突；禁止覆盖或回退。"));
			}
		}
		if (LegacyState == ELegacyIdentityState::Missing
			&& !WriteMigrationFence(LegacyPath, false, OutError))
		{
			SecureReset(RecoveredPrivateKey);
			return false;
		}
		if (!bSame)
		{
			if (!WriteIdentityFile(
					ProtectedPath,
					RecoveredPrivateKey,
					RecoveredInfo,
					true,
					OutError))
			{
				SecureReset(RecoveredPrivateKey);
				return false;
			}
			ExistingInfo = RecoveredInfo;
		}
		if (LegacyState == ELegacyIdentityState::Plaintext
			&& !WriteMigrationFence(LegacyPath, true, OutError))
		{
			SecureReset(RecoveredPrivateKey);
			return false;
		}
		SecureReset(RecoveredPrivateKey);
		OutInfo = MoveTemp(ExistingInfo);
		return true;
	}
	if (LegacyState == ELegacyIdentityState::Missing
		&& !WriteMigrationFence(LegacyPath, false, OutError))
	{
		SecureReset(RecoveredPrivateKey);
		return false;
	}
	if (!WriteIdentityFile(
			ProtectedPath,
			RecoveredPrivateKey,
			RecoveredInfo,
			false,
			OutError))
	{
		SecureReset(RecoveredPrivateKey);
		return false;
	}

	TArray<uint8> VerifiedPrivateKey;
	FAkUGCPublisherIdentityInfo VerifiedInfo;
	FString VerificationError;
	const bool bVerified =
		LoadIdentityFile(
			ProtectedPath,
			VerifiedPrivateKey,
			VerifiedInfo,
			&VerificationError)
		&& PrivateKeysEqual(VerifiedPrivateKey, RecoveredPrivateKey)
		&& IdentityInfoEqual(VerifiedInfo, RecoveredInfo);
	SecureReset(VerifiedPrivateKey);
	SecureReset(RecoveredPrivateKey);
	if (!bVerified)
	{
		return Fail(
			OutError,
			TEXT("恢复身份写入后校验失败：") + VerificationError);
	}

	if (LegacyState == ELegacyIdentityState::Plaintext
		&& !WriteMigrationFence(LegacyPath, true, OutError))
	{
		return false;
	}
	OutInfo = MoveTemp(VerifiedInfo);
	return true;
#endif
}

bool FAkUGCPublisherKeyStore::RotateWithRecovery(
	const FString& RecoveryOutputPath,
	TConstArrayView<uint8> PassphraseUtf8,
	FAkUGCPublisherIdentityInfo& OutInfo,
	FString* OutError) const
{
	using namespace AkUGCPublisherKeyStorePrivate;
	using AkUGCPublisherKeyStorePrivate::Fail;
	using AkUGCPublisherKeyStorePrivate::SecureReset;
	OutInfo = FAkUGCPublisherIdentityInfo();
	if (OutError)
	{
		OutError->Reset();
	}
	if (Root.IsEmpty())
	{
		return Fail(OutError, TEXT("发布身份目录不能为空。"));
	}
	FString FullRecoveryPath;
	if (!NormalizeRecoveryPath(
			RecoveryOutputPath,
			Root,
			FullRecoveryPath,
			OutError)
		|| !ValidateRecoveryPassphrase(PassphraseUtf8, OutError))
	{
		return false;
	}
#if !PLATFORM_WINDOWS
	return Fail(OutError, TEXT("当前平台不支持轮换发布身份。"));
#else
	FPublisherIdentityFileLock StoreLock;
	if (!StoreLock.Acquire(Root, OutError))
	{
		return false;
	}

	TArray<uint8> PreviousPrivateKey;
	FString PreviousPublicKey;
	FAkUGCPublisherIdentityInfo PreviousInfo;
	if (!LoadOrCreateLocked(
			PreviousPrivateKey,
			PreviousPublicKey,
			&PreviousInfo,
			OutError))
	{
		SecureReset(PreviousPrivateKey);
		return false;
	}
	if (PreviousInfo.Rotations.Num() >= MaxRotationRecords)
	{
		SecureReset(PreviousPrivateKey);
		return Fail(OutError, TEXT("发布身份轮换次数已达到本地格式上限。"));
	}

	FAkUGCLogicPackKeyPair Generated;
	if (!FAkUGCLogicPackSigner::GenerateKeyPair(Generated, OutError))
	{
		SecureReset(PreviousPrivateKey);
		return false;
	}
	TArray<uint8> NewPrivateKey;
	const bool bPrivateKeyValid =
		HexToPrivateKeyBytes(Generated.PrivateKey, NewPrivateKey);
	SecureReset(Generated.PrivateKey);
	if (!bPrivateKeyValid)
	{
		SecureReset(PreviousPrivateKey);
		return Fail(OutError, TEXT("新轮换私钥格式非法。"));
	}

	FAkUGCPublisherIdentityInfo NewInfo = PreviousInfo;
	FAkUGCPublisherKeyRotation Rotation;
	Rotation.Sequence = PreviousInfo.KeyVersion;
	const int64 PreviousTimestamp =
		PreviousInfo.Rotations.IsEmpty()
			? 0
			: PreviousInfo.Rotations.Last().CreatedAtUnixSeconds;
	Rotation.CreatedAtUnixSeconds = FMath::Max(
		FDateTime::UtcNow().ToUnixTimestamp(),
		PreviousTimestamp + 1);
	Rotation.PreviousPublicKey = PreviousInfo.PublicKey;
	Rotation.NewPublicKey = Generated.PublicKey;
	const FString Message = RotationMessage(Rotation);
	if (!SignMessage(
			PreviousPrivateKey,
			Message,
			Rotation.PreviousSignature,
			OutError)
		|| !SignMessage(
			NewPrivateKey,
			Message,
			Rotation.NewSignature,
			OutError))
	{
		SecureReset(PreviousPrivateKey);
		SecureReset(NewPrivateKey);
		return false;
	}
	NewInfo.KeyVersion = PreviousInfo.KeyVersion + 1;
	NewInfo.PublicKey = Generated.PublicKey;
	NewInfo.Rotations.Add(MoveTemp(Rotation));
	if (!VerifyIdentityHistoryInternal(NewInfo, OutError))
	{
		SecureReset(PreviousPrivateKey);
		SecureReset(NewPrivateKey);
		return false;
	}

	if (!WriteNewRecoveryBundle(
			FullRecoveryPath,
			NewPrivateKey,
			NewInfo,
			PassphraseUtf8,
			OutError))
	{
		SecureReset(PreviousPrivateKey);
		SecureReset(NewPrivateKey);
		return false;
	}
	if (!WriteIdentityFile(
			IdentityPath(),
			NewPrivateKey,
			NewInfo,
			true,
			OutError))
	{
		const bool bRemoved = IFileManager::Get().Delete(
			*FullRecoveryPath,
			false,
			false,
			true);
		if (!bRemoved && OutError)
		{
			*OutError +=
				TEXT(" 新身份未启用，但已生成的恢复包无法删除，请隔离：")
				+ FullRecoveryPath;
		}
		SecureReset(PreviousPrivateKey);
		SecureReset(NewPrivateKey);
		return false;
	}
	SecureReset(PreviousPrivateKey);

	TArray<uint8> VerifiedPrivateKey;
	FAkUGCPublisherIdentityInfo VerifiedInfo;
	FString VerificationError;
	const bool bVerified =
		LoadIdentityFile(
			IdentityPath(),
			VerifiedPrivateKey,
			VerifiedInfo,
			&VerificationError)
		&& PrivateKeysEqual(VerifiedPrivateKey, NewPrivateKey)
		&& IdentityInfoEqual(VerifiedInfo, NewInfo);
	SecureReset(VerifiedPrivateKey);
	SecureReset(NewPrivateKey);
	if (!bVerified)
	{
		return Fail(
			OutError,
			TEXT("轮换身份提交后校验失败；请保留新恢复包并停止发布：")
				+ VerificationError);
	}
	OutInfo = MoveTemp(VerifiedInfo);
	return true;
#endif
}

bool FAkUGCPublisherKeyStore::VerifyIdentityHistory(
	const FAkUGCPublisherIdentityInfo& Info,
	FString* OutError)
{
	using namespace AkUGCPublisherKeyStorePrivate;
	if (OutError)
	{
		OutError->Reset();
	}
	return VerifyIdentityHistoryInternal(Info, OutError);
}

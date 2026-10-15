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

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <wincrypt.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace
{
	constexpr int32 PublisherIdentityVersion = 1;
	constexpr int32 Ed25519PrivateKeyBytes = 32;
	constexpr int64 MaxIdentityFileBytes = 64 * 1024;
	constexpr int64 MaxLegacyKeyFileBytes = 512;
	const TCHAR* IdentityFileName = TEXT("publisher-identity.json");
	const TCHAR* LegacyPrivateKeyFileName = TEXT("private.key");
	const TCHAR* LockFileName = TEXT("publisher-identity.lock");
	const TCHAR* ProtectionName = TEXT("windows-dpapi-current-user-v1");
	const TCHAR* LegacyMigrationFence =
		TEXT("{\"version\":1,\"migratedTo\":\"publisher-identity.json\",\"containsPrivateKey\":false}");

	enum class ELegacyIdentityState : uint8
	{
		Missing,
		Plaintext,
		MigrationFence,
	};

	bool Fail(FString* OutError, const FString& Message);

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

	bool HexToPrivateKeyBytes(const FString& Hex, TArray<uint8>& OutPrivateKey)
	{
		OutPrivateKey.Reset();
		if (Hex.Len() != Ed25519PrivateKeyBytes * 2)
		{
			return false;
		}
		OutPrivateKey.SetNumUninitialized(Ed25519PrivateKeyBytes);
		for (int32 Index = 0; Index < Ed25519PrivateKeyBytes; ++Index)
		{
			const int32 High = HexNibble(Hex[Index * 2]);
			const int32 Low = HexNibble(Hex[Index * 2 + 1]);
			if (High < 0 || Low < 0)
			{
				SecureReset(OutPrivateKey);
				return false;
			}
			OutPrivateKey[Index] = static_cast<uint8>((High << 4) | Low);
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
		OutText.Reset();
		const int64 Size = IFileManager::Get().FileSize(*Path);
		if (Size <= 0 || Size > MaxBytes)
		{
			return Fail(OutError, FString::Printf(TEXT("%s缺失、为空或大小异常。"), Label));
		}
		if (!FFileHelper::LoadFileToString(OutText, *Path))
		{
			return Fail(OutError, FString::Printf(TEXT("无法读取%s。"), Label));
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
		const FString& PublicKey,
		TConstArrayView<uint8> ProtectedPrivateKey,
		FString& OutJson,
		FString* OutError)
	{
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetNumberField(TEXT("version"), PublisherIdentityVersion);
		Object->SetStringField(TEXT("protection"), ProtectionName);
		Object->SetStringField(TEXT("publicKey"), PublicKey);
		Object->SetStringField(
			TEXT("protectedPrivateKey"),
			FBase64::Encode(ProtectedPrivateKey.GetData(), ProtectedPrivateKey.Num()));
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
		FString& OutPublicKey,
		TArray<uint8>& OutProtectedPrivateKey,
		FString* OutError)
	{
		OutPublicKey.Reset();
		OutProtectedPrivateKey.Reset();
		TSharedPtr<FJsonObject> Object;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Object)
			|| !Object.IsValid()
			|| Object->Values.Num() != 4)
		{
			return Fail(OutError, TEXT("受保护发布身份格式错误，禁止自动替换。"));
		}

		double Version = 0.0;
		FString Protection;
		FString EncodedPrivateKey;
		if (!Object->TryGetNumberField(TEXT("version"), Version)
			|| Version != PublisherIdentityVersion
			|| !Object->TryGetStringField(TEXT("protection"), Protection)
			|| Protection != ProtectionName
			|| !Object->TryGetStringField(TEXT("publicKey"), OutPublicKey)
			|| !Object->TryGetStringField(TEXT("protectedPrivateKey"), EncodedPrivateKey)
			|| OutPublicKey.Len() != 64
			|| OutPublicKey != OutPublicKey.ToLower())
		{
			OutPublicKey.Reset();
			return Fail(OutError, TEXT("受保护发布身份字段错误或版本不兼容，禁止自动替换。"));
		}
		for (const TCHAR Character : OutPublicKey)
		{
			if (HexNibble(Character) < 0)
			{
				OutPublicKey.Reset();
				return Fail(OutError, TEXT("受保护发布身份的公钥格式错误。"));
			}
		}
		if (!FBase64::Decode(EncodedPrivateKey, OutProtectedPrivateKey)
			|| OutProtectedPrivateKey.IsEmpty()
			|| FBase64::Encode(
				OutProtectedPrivateKey.GetData(),
				OutProtectedPrivateKey.Num()) != EncodedPrivateKey)
		{
			OutPublicKey.Reset();
			OutProtectedPrivateKey.Reset();
			return Fail(OutError, TEXT("受保护发布身份的密文格式错误。"));
		}
		return true;
	}

	bool LoadIdentityFile(
		const FString& Path,
		TArray<uint8>& OutPrivateKey,
		FString& OutPublicKey,
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
		if (!ParseIdentity(Json, OutPublicKey, ProtectedPrivateKey, OutError))
		{
			return false;
		}
		const bool bUnprotected = UnprotectPrivateKey(
			ProtectedPrivateKey,
			OutPublicKey,
			OutPrivateKey,
			OutError);
		SecureReset(ProtectedPrivateKey);
		return bUnprotected;
	}

	bool WriteNewIdentityFile(
		const FString& Path,
		TConstArrayView<uint8> PrivateKey,
		const FString& PublicKey,
		FString* OutError)
	{
		if (IFileManager::Get().FileExists(*Path)
			|| IFileManager::Get().DirectoryExists(*Path))
		{
			return Fail(OutError, TEXT("受保护发布身份已存在，不会覆盖。"));
		}

		TArray<uint8> ProtectedPrivateKey;
		if (!ProtectPrivateKey(PrivateKey, PublicKey, ProtectedPrivateKey, OutError))
		{
			return false;
		}
		FString Json;
		const bool bSerialized = SerializeIdentity(
			PublicKey,
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
			&& IFileManager::Get().Move(*Path, *Temporary, false, false, false, true);
		if (!bWritten)
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
			&& FMemory::Memcmp(A.GetData(), B.GetData(), A.Num()) == 0;
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
	return Root.IsEmpty() ? FString() : FPaths::Combine(Root, IdentityFileName);
}

FString FAkUGCPublisherKeyStore::LegacyPrivateKeyPath() const
{
	return Root.IsEmpty() ? FString() : FPaths::Combine(Root, LegacyPrivateKeyFileName);
}

bool FAkUGCPublisherKeyStore::LoadOrCreate(
	TArray<uint8>& OutPrivateKey,
	FString& OutPublicKey,
	FString* OutError) const
{
	SecureReset(OutPrivateKey);
	OutPublicKey.Reset();
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
		if (!LoadIdentityFile(
				ProtectedPath,
				OutPrivateKey,
				OutPublicKey,
				OutError))
		{
			SecureReset(OutPrivateKey);
			OutPublicKey.Reset();
			return false;
		}
		return ValidateLoadedIdentity(OutPrivateKey, OutPublicKey);
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
			FString WriteError;
			if (!WriteNewIdentityFile(
					ProtectedPath,
					LegacyPrivateKey,
					LegacyPublicKey,
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
		const bool bWritten = WriteNewIdentityFile(
			ProtectedPath,
			GeneratedPrivateKey,
			Generated.PublicKey,
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
#endif
}

bool FAkUGCPublisherKeyStore::GetOrCreatePublicKey(
	FString& OutPublicKey,
	FString* OutError) const
{
	TArray<uint8> PrivateKey;
	const bool bLoaded = LoadOrCreate(PrivateKey, OutPublicKey, OutError);
	SecureReset(PrivateKey);
	return bLoaded;
}

bool FAkUGCPublisherKeyStore::Sign(
	const FAkUGCLogicPackManifest& Manifest,
	FAkUGCLogicPackSignature& OutSignature,
	FString* OutError) const
{
	TArray<uint8> PrivateKey;
	FString PublicKey;
	if (!LoadOrCreate(PrivateKey, PublicKey, OutError))
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

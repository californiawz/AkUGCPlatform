#include "Registry/AkUGCReleaseRegistryClient.h"

#include <initializer_list>

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Pack/AkUGCLogicPackHasher.h"
#include "Pack/AkUGCLogicPackLoader.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Validation/AkUGCInputBudget.h"

namespace AkUGCReleaseRegistryClientPrivate
{
	constexpr double MaxJsonSafeInteger = 9007199254740991.0;

	bool Fail(FString& OutError, const FString& Message)
	{
		OutError = Message;
		return false;
	}

	bool IsLowerHex(const FString& Value, int32 Characters)
	{
		if (Value.Len() != Characters)
		{
			return false;
		}
		for (const TCHAR Character : Value)
		{
			if (!((Character >= TEXT('0') && Character <= TEXT('9'))
				|| (Character >= TEXT('a') && Character <= TEXT('f'))))
			{
				return false;
			}
		}
		return true;
	}

	bool ValidateFields(
		const TSharedPtr<FJsonObject>& Object,
		std::initializer_list<const TCHAR*> Required,
		std::initializer_list<const TCHAR*> Optional,
		const TCHAR* Label,
		FString& OutError)
	{
		if (!Object.IsValid())
		{
			return Fail(
				OutError,
				FString::Printf(TEXT("%s 不是 JSON 对象。"), Label));
		}
		TSet<FString> Allowed;
		for (const TCHAR* Name : Required)
		{
			Allowed.Add(Name);
			if (!Object->HasField(Name))
			{
				return Fail(
					OutError,
					FString::Printf(TEXT("%s 缺少字段 %s。"), Label, Name));
			}
		}
		for (const TCHAR* Name : Optional)
		{
			Allowed.Add(Name);
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
		{
			if (!Allowed.Contains(Pair.Key))
			{
				return Fail(
					OutError,
					FString::Printf(TEXT("%s 包含未知字段。"), Label));
			}
		}
		return true;
	}

	bool ReadArtifact(
		const FString& SharePath,
		const FAkUGCWorkEntry& Entry,
		const FGuid& ExpectedProjectId,
		TArray<uint8>& OutArtifact,
		FString& OutError)
	{
		OutArtifact.Reset();
		TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*SharePath));
		const int64 Size = Reader ? Reader->TotalSize() : -1;
		if (Size <= 0
			|| Size > AkUGCInputBudget::MaxLogicPackBytes
			|| Size > MAX_int32)
		{
			return Fail(
				OutError,
				TEXT("待上传作品不存在、为空或超过 16 MiB。"));
		}
		OutArtifact.SetNumUninitialized(static_cast<int32>(Size));
		Reader->Serialize(OutArtifact.GetData(), Size);
		if (Reader->IsError() || Reader->Tell() != Size)
		{
			OutArtifact.Reset();
			return Fail(OutError, TEXT("待上传作品未能完整读取。"));
		}

		FString Json;
		FFileHelper::BufferToString(Json, OutArtifact.GetData(), OutArtifact.Num());
		FTCHARToUTF8 RoundTrip(*Json);
		if (RoundTrip.Length() != OutArtifact.Num()
			|| FMemory::Memcmp(
				RoundTrip.Get(),
				OutArtifact.GetData(),
				OutArtifact.Num()) != 0)
		{
			OutArtifact.Reset();
			return Fail(
				OutError,
				TEXT("待上传作品不是规范 UTF-8 文本。"));
		}
		const FAkUGCLogicPackLoadResult Verified =
			FAkUGCLogicPackLoader::LoadVerified(Json, Entry.TrustedPublicKey);
		if (!Verified.bSucceeded
			|| Verified.Pack.Manifest.ReleaseId != Entry.ReleaseId
			|| Verified.Pack.Manifest.ProjectId != ExpectedProjectId
			|| Verified.Pack.Document.Manifest.ProjectId != ExpectedProjectId
			|| Verified.Pack.Document.Manifest.DisplayName != Entry.DisplayName
			|| Verified.Pack.Manifest.ContentHash != Entry.ContentHash
			|| Verified.Pack.Signature.PublicKey != Entry.TrustedPublicKey)
		{
			OutArtifact.Reset();
			return Fail(
				OutError,
				TEXT("待上传作品与最近一次本地签名发布不一致，已拒绝上传。"));
		}
		return true;
	}

	bool StartCommon(
		const FString& BaseUrl,
		const FString& BearerToken,
		const FGuid& ReleaseId,
		EAkUGCEditorHttpMethod Method,
		const TCHAR* Suffix,
		float TimeoutSeconds,
		FAkUGCEditorHttpRequestPtr& OutRequest,
		FString& OutError)
	{
		OutRequest.Reset();
		FString Normalized;
		if (!FAkUGCReleaseRegistryClient::NormalizeBaseUrl(
				BaseUrl,
				Normalized,
				OutError)
			|| !FAkUGCReleaseRegistryClient::ValidateBearerToken(
				BearerToken,
				OutError))
		{
			return false;
		}
		if (!ReleaseId.IsValid())
		{
			return Fail(OutError, TEXT("Release ID 无效。"));
		}
		FAkUGCEditorHttpRequestSpec Spec;
		Spec.Method = Method;
		Spec.Url = Normalized
			+ TEXT("/v1/releases/")
			+ ReleaseId.ToString(EGuidFormats::DigitsWithHyphensLower)
			+ Suffix;
		Spec.BearerToken = BearerToken;
		Spec.TimeoutSeconds = TimeoutSeconds;
		Spec.MaxResponseBytes =
			FAkUGCReleaseRegistryClient::MaxControlResponseBytes;
		Spec.bAllowLoopbackHttpWithBearer = true;
		OutRequest = FAkUGCEditorHttpTransport::Start(MoveTemp(Spec));
		if (!OutRequest.IsValid())
		{
			return Fail(
				OutError,
				TEXT("Registry 传输不可用、忙碌或请求配置无效。"));
		}
		return true;
	}

	FAkUGCEditorHttpRequestPtr StartProjectRequest(
		const FString& BaseUrl,
		const FString& BearerToken,
		const FGuid& ProjectId,
		EAkUGCEditorHttpMethod Method,
		const FString& Suffix,
		TArray<uint8> Body,
		const FString& ContentType,
		const FString& IdempotencyKey,
		bool bAllowQuery,
		float TimeoutSeconds,
		FString& OutError)
	{
		FString Normalized;
		if (!FAkUGCReleaseRegistryClient::NormalizeBaseUrl(
				BaseUrl,
				Normalized,
				OutError)
			|| !FAkUGCReleaseRegistryClient::ValidateBearerToken(
				BearerToken,
				OutError)
			|| !ProjectId.IsValid())
		{
			if (OutError.IsEmpty())
			{
				OutError = TEXT("Project ID 无效。");
			}
			return nullptr;
		}
		FAkUGCEditorHttpRequestSpec Spec;
		Spec.Method = Method;
		Spec.Url = Normalized
			+ TEXT("/v1/projects/")
			+ ProjectId.ToString(EGuidFormats::DigitsWithHyphensLower)
			+ Suffix;
		Spec.BearerToken = BearerToken;
		Spec.ContentType = ContentType;
		Spec.IdempotencyKey = IdempotencyKey;
		Spec.Body = MoveTemp(Body);
		Spec.TimeoutSeconds = TimeoutSeconds;
		Spec.MaxResponseBytes =
			FAkUGCReleaseRegistryClient::MaxControlResponseBytes;
		Spec.bAllowLoopbackHttpWithBearer = true;
		Spec.bAllowQuery = bAllowQuery;
		FAkUGCEditorHttpRequestPtr Request =
			FAkUGCEditorHttpTransport::Start(MoveTemp(Spec));
		if (!Request.IsValid())
		{
			OutError =
				TEXT("Registry 传输不可用、忙碌或请求配置无效。");
		}
		return Request;
	}

	bool SerializeJsonObject(
		const TSharedRef<FJsonObject>& Object,
		TArray<uint8>& OutBody,
		FString& OutError)
	{
		FString Json;
		const TSharedRef<TJsonWriter<>> Writer =
			TJsonWriterFactory<>::Create(&Json);
		if (!FJsonSerializer::Serialize(Object, Writer))
		{
			return Fail(OutError, TEXT("Registry 请求 JSON 序列化失败。"));
		}
		FTCHARToUTF8 Bytes(*Json);
		if (Bytes.Length() <= 0 || Bytes.Length() > 4096)
		{
			return Fail(OutError, TEXT("Registry 请求 JSON 超过控制面预算。"));
		}
		OutBody.Append(
			reinterpret_cast<const uint8*>(Bytes.Get()),
			Bytes.Length());
		return true;
	}

	bool IsSafeSubject(const FString& Value)
	{
		if (Value.IsEmpty() || Value.Len() > 128)
		{
			return false;
		}
		for (int32 Index = 0; Index < Value.Len(); ++Index)
		{
			const TCHAR Character = Value[Index];
			const bool bAllowed =
				(Character >= TEXT('a') && Character <= TEXT('z'))
				|| (Character >= TEXT('A') && Character <= TEXT('Z'))
				|| (Character >= TEXT('0') && Character <= TEXT('9'))
				|| (Index > 0
					&& (Character == TEXT('.')
						|| Character == TEXT('_')
						|| Character == TEXT(':')
						|| Character == TEXT('-')));
			if (!bAllowed)
			{
				return false;
			}
		}
		return true;
	}

	bool IsProjectRole(const FString& Value)
	{
		return Value == TEXT("owner")
			|| Value == TEXT("editor")
			|| Value == TEXT("viewer");
	}

	bool IsTimestamp(const FString& Value)
	{
		FDateTime Parsed;
		return Value.Len() >= 20
			&& Value.Len() <= 64
			&& FDateTime::ParseIso8601(*Value, Parsed);
	}

	bool ParseCanonicalGuid(const FString& Value, FGuid& OutGuid)
	{
		return FGuid::Parse(Value, OutGuid)
			&& OutGuid.IsValid()
			&& Value == OutGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
	}

	bool ParseProject(
		const TSharedPtr<FJsonObject>& Object,
		const FGuid& ExpectedProjectId,
		FAkUGCRegistryProjectView& OutProject,
		FString& OutError)
	{
		if (!ValidateFields(
				Object,
				{
					TEXT("projectId"),
					TEXT("displayName"),
					TEXT("ownerSubject"),
					TEXT("visibility"),
					TEXT("revision"),
					TEXT("createdAt"),
					TEXT("updatedAt")
				},
				{},
				TEXT("Registry project"),
				OutError))
		{
			return false;
		}
		FString ProjectId;
		double Revision = -1;
		if (!Object->TryGetStringField(TEXT("projectId"), ProjectId)
			|| !Object->TryGetStringField(
				TEXT("displayName"),
				OutProject.DisplayName)
			|| !Object->TryGetStringField(
				TEXT("ownerSubject"),
				OutProject.OwnerSubject)
			|| !Object->TryGetStringField(
				TEXT("visibility"),
				OutProject.Visibility)
			|| !Object->TryGetNumberField(TEXT("revision"), Revision)
			|| !Object->TryGetStringField(
				TEXT("createdAt"),
				OutProject.CreatedAt)
			|| !Object->TryGetStringField(
				TEXT("updatedAt"),
				OutProject.UpdatedAt)
			|| !ParseCanonicalGuid(ProjectId, OutProject.ProjectId)
			|| OutProject.ProjectId != ExpectedProjectId
			|| !FAkUGCReleaseRegistryClient::ValidateProjectDisplayName(
				OutProject.DisplayName,
				OutError)
			|| !IsSafeSubject(OutProject.OwnerSubject)
			|| (OutProject.Visibility != TEXT("private")
				&& OutProject.Visibility != TEXT("public"))
			|| !FMath::IsFinite(Revision)
			|| Revision < 1
			|| Revision > MaxJsonSafeInteger
			|| FMath::FloorToDouble(Revision) != Revision
			|| !IsTimestamp(OutProject.CreatedAt)
			|| !IsTimestamp(OutProject.UpdatedAt))
		{
			if (OutError.IsEmpty())
			{
				OutError = TEXT("Registry project 字段类型无效或与当前项目不一致。");
			}
			return false;
		}
		OutProject.Revision = static_cast<int64>(Revision);
		return true;
	}

	bool ParseMember(
		const TSharedPtr<FJsonObject>& Object,
		const FGuid& ExpectedProjectId,
		FAkUGCRegistryMemberView& OutMember,
		FString& OutError)
	{
		if (!ValidateFields(
				Object,
				{
					TEXT("projectId"),
					TEXT("subject"),
					TEXT("role"),
					TEXT("addedAt"),
					TEXT("updatedAt")
				},
				{},
				TEXT("Registry project member"),
				OutError))
		{
			return false;
		}
		FString ProjectId;
		if (!Object->TryGetStringField(TEXT("projectId"), ProjectId)
			|| !Object->TryGetStringField(TEXT("subject"), OutMember.Subject)
			|| !Object->TryGetStringField(TEXT("role"), OutMember.Role)
			|| !Object->TryGetStringField(TEXT("addedAt"), OutMember.AddedAt)
			|| !Object->TryGetStringField(TEXT("updatedAt"), OutMember.UpdatedAt)
			|| !ParseCanonicalGuid(ProjectId, OutMember.ProjectId)
			|| OutMember.ProjectId != ExpectedProjectId
			|| !IsSafeSubject(OutMember.Subject)
			|| !IsProjectRole(OutMember.Role)
			|| !IsTimestamp(OutMember.AddedAt)
			|| !IsTimestamp(OutMember.UpdatedAt))
		{
			return Fail(
				OutError,
				TEXT("Registry project member 字段无效或与当前项目不一致。"));
		}
		return true;
	}

	bool ParseReleaseMetadata(
		const TSharedPtr<FJsonObject>& Object,
		const FGuid& ExpectedProjectId,
		FAkUGCReleaseRegistryView& OutRelease,
		FString& OutError)
	{
		if (!ValidateFields(
				Object,
				{
					TEXT("releaseId"),
					TEXT("projectId"),
					TEXT("schemaVersion"),
					TEXT("ownerSubject"),
					TEXT("submittedBySubject"),
					TEXT("publisherPublicKey"),
					TEXT("contentHash"),
					TEXT("artifactSha256"),
					TEXT("artifactSize"),
					TEXT("status"),
					TEXT("revision"),
					TEXT("submittedAt"),
					TEXT("updatedAt")
				},
				{
					TEXT("reviewReason"),
					TEXT("reviewedAt"),
					TEXT("publishedAt"),
					TEXT("revokedAt")
				},
				TEXT("Registry release"),
				OutError))
		{
			return false;
		}

		FString ReleaseId;
		FString ProjectId;
		FString UpdatedAt;
		double SchemaVersion = -1;
		double ArtifactSize = -1;
		double Revision = -1;
		if (!Object->TryGetStringField(TEXT("releaseId"), ReleaseId)
			|| !Object->TryGetStringField(TEXT("projectId"), ProjectId)
			|| !Object->TryGetNumberField(TEXT("schemaVersion"), SchemaVersion)
			|| !Object->TryGetStringField(
				TEXT("ownerSubject"),
				OutRelease.OwnerSubject)
			|| !Object->TryGetStringField(
				TEXT("submittedBySubject"),
				OutRelease.SubmittedBySubject)
			|| !Object->TryGetStringField(
				TEXT("publisherPublicKey"),
				OutRelease.PublisherPublicKey)
			|| !Object->TryGetStringField(
				TEXT("contentHash"),
				OutRelease.ContentHash)
			|| !Object->TryGetStringField(
				TEXT("artifactSha256"),
				OutRelease.ArtifactSha256)
			|| !Object->TryGetNumberField(TEXT("artifactSize"), ArtifactSize)
			|| !Object->TryGetStringField(TEXT("status"), OutRelease.Status)
			|| !Object->TryGetNumberField(TEXT("revision"), Revision)
			|| !Object->TryGetStringField(
				TEXT("submittedAt"),
				OutRelease.SubmittedAt)
			|| !Object->TryGetStringField(TEXT("updatedAt"), UpdatedAt)
			|| !ParseCanonicalGuid(ReleaseId, OutRelease.ReleaseId)
			|| !ParseCanonicalGuid(ProjectId, OutRelease.ProjectId)
			|| (ExpectedProjectId.IsValid()
				&& OutRelease.ProjectId != ExpectedProjectId)
			|| !IsSafeSubject(OutRelease.OwnerSubject)
			|| !IsSafeSubject(OutRelease.SubmittedBySubject)
			|| !IsLowerHex(OutRelease.PublisherPublicKey, 64)
			|| !IsLowerHex(OutRelease.ContentHash, 64)
			|| !IsLowerHex(OutRelease.ArtifactSha256, 64)
			|| !FMath::IsFinite(SchemaVersion)
			|| SchemaVersion < 0
			|| SchemaVersion > MAX_int32
			|| FMath::FloorToDouble(SchemaVersion) != SchemaVersion
			|| !FMath::IsFinite(ArtifactSize)
			|| ArtifactSize < 1
			|| ArtifactSize > AkUGCInputBudget::MaxLogicPackBytes
			|| FMath::FloorToDouble(ArtifactSize) != ArtifactSize
			|| !FMath::IsFinite(Revision)
			|| Revision < 1
			|| Revision > MaxJsonSafeInteger
			|| FMath::FloorToDouble(Revision) != Revision
			|| !IsTimestamp(OutRelease.SubmittedAt)
			|| !IsTimestamp(UpdatedAt)
			|| (OutRelease.Status != TEXT("submitted")
				&& OutRelease.Status != TEXT("approved")
				&& OutRelease.Status != TEXT("rejected")
				&& OutRelease.Status != TEXT("published")
				&& OutRelease.Status != TEXT("revoked")))
		{
			return Fail(
				OutError,
				TEXT("Registry release 字段无效或与当前项目不一致。"));
		}
		for (const TCHAR* Name : {
			TEXT("reviewedAt"),
			TEXT("publishedAt"),
			TEXT("revokedAt")})
		{
			FString Value;
			if (Object->HasField(Name)
				&& (!Object->TryGetStringField(Name, Value)
					|| !IsTimestamp(Value)))
			{
				return Fail(
					OutError,
					TEXT("Registry release 可选时间字段无效。"));
			}
		}
		FString ReviewReason;
		if (Object->HasField(TEXT("reviewReason"))
			&& (!Object->TryGetStringField(TEXT("reviewReason"), ReviewReason)
				|| FTCHARToUTF8(*ReviewReason).Length() > 1024))
		{
			return Fail(
				OutError,
				TEXT("Registry release 审核原因字段无效。"));
		}
		OutRelease.Revision = static_cast<int64>(Revision);
		OutRelease.ArtifactSize = static_cast<int64>(ArtifactSize);
		return true;
	}

	bool ParseRelease(
		const TSharedPtr<FJsonObject>& Object,
		const FAkUGCWorkEntry& Expected,
		const FGuid& ExpectedProjectId,
		FAkUGCReleaseRegistryView& OutRelease,
		FString& OutError)
	{
		if (!ParseReleaseMetadata(
				Object,
				ExpectedProjectId,
				OutRelease,
				OutError))
		{
			return false;
		}
		if (OutRelease.ReleaseId != Expected.ReleaseId
			|| OutRelease.PublisherPublicKey != Expected.TrustedPublicKey
			|| OutRelease.ContentHash != Expected.ContentHash)
		{
			return Fail(
				OutError,
				TEXT("Registry release 与本地签名作品不一致。"));
		}
		return true;
	}

	bool ParseBuild(
		const TSharedPtr<FJsonObject>& Object,
		const FGuid& ExpectedReleaseId,
		const FGuid& ExpectedProjectId,
		const FString& ExpectedArtifactSha256,
		int64 ExpectedArtifactSize,
		FAkUGCBuildRegistryView& OutBuild,
		FString& OutError)
	{
		if (!ValidateFields(
				Object,
				{
					TEXT("releaseId"),
					TEXT("projectId"),
					TEXT("status"),
					TEXT("attempt"),
					TEXT("revision"),
					TEXT("retryable"),
					TEXT("inputArtifactSha256"),
					TEXT("createdAt"),
					TEXT("updatedAt")
				},
				{
					TEXT("outputArtifactSha256"),
					TEXT("outputArtifactSize"),
					TEXT("errorCode"),
					TEXT("startedAt"),
					TEXT("finishedAt")
				},
				TEXT("Registry build"),
				OutError))
		{
			return false;
		}

		FString ReleaseId;
		FString ProjectId;
		double Attempt = -1;
		double Revision = -1;
		double OutputArtifactSize = -1;
		if (!Object->TryGetStringField(TEXT("releaseId"), ReleaseId)
			|| !Object->TryGetStringField(TEXT("projectId"), ProjectId)
			|| !Object->TryGetStringField(TEXT("status"), OutBuild.Status)
			|| !Object->TryGetNumberField(TEXT("attempt"), Attempt)
			|| !Object->TryGetNumberField(TEXT("revision"), Revision)
			|| !Object->TryGetBoolField(
				TEXT("retryable"),
				OutBuild.bRetryable)
			|| !Object->TryGetStringField(
				TEXT("inputArtifactSha256"),
				OutBuild.InputArtifactSha256)
			|| !Object->TryGetStringField(
				TEXT("createdAt"),
				OutBuild.CreatedAt)
			|| !Object->TryGetStringField(
				TEXT("updatedAt"),
				OutBuild.UpdatedAt)
			|| !ParseCanonicalGuid(ReleaseId, OutBuild.ReleaseId)
			|| !ParseCanonicalGuid(ProjectId, OutBuild.ProjectId)
			|| OutBuild.ReleaseId != ExpectedReleaseId
			|| OutBuild.ProjectId != ExpectedProjectId
			|| OutBuild.InputArtifactSha256 != ExpectedArtifactSha256
			|| !IsLowerHex(OutBuild.InputArtifactSha256, 64)
			|| ExpectedArtifactSize < 1
			|| ExpectedArtifactSize > AkUGCInputBudget::MaxLogicPackBytes
			|| !FMath::IsFinite(Attempt)
			|| Attempt < 0
			|| Attempt > 3
			|| FMath::FloorToDouble(Attempt) != Attempt
			|| !FMath::IsFinite(Revision)
			|| Revision < 1
			|| Revision > MaxJsonSafeInteger
			|| FMath::FloorToDouble(Revision) != Revision
			|| !IsTimestamp(OutBuild.CreatedAt)
			|| !IsTimestamp(OutBuild.UpdatedAt)
			|| (OutBuild.Status != TEXT("queued")
				&& OutBuild.Status != TEXT("running")
				&& OutBuild.Status != TEXT("succeeded")
				&& OutBuild.Status != TEXT("failed")))
		{
			return Fail(
				OutError,
				TEXT("Registry build 字段无效或与本地签名制品不一致。"));
		}
		OutBuild.Attempt = static_cast<int64>(Attempt);
		OutBuild.Revision = static_cast<int64>(Revision);

		const auto ReadOptionalString =
			[&Object](const TCHAR* Name, FString& OutValue)
			{
				return !Object->HasField(Name)
					|| Object->TryGetStringField(Name, OutValue);
			};
		if (!ReadOptionalString(
				TEXT("outputArtifactSha256"),
				OutBuild.OutputArtifactSha256)
			|| !ReadOptionalString(
				TEXT("errorCode"),
				OutBuild.ErrorCode)
			|| !ReadOptionalString(
				TEXT("startedAt"),
				OutBuild.StartedAt)
			|| !ReadOptionalString(
				TEXT("finishedAt"),
				OutBuild.FinishedAt))
		{
			return Fail(
				OutError,
				TEXT("Registry build 可选字符串字段无效。"));
		}
		if (Object->HasField(TEXT("outputArtifactSize")))
		{
			if (!Object->TryGetNumberField(
					TEXT("outputArtifactSize"),
					OutputArtifactSize)
				|| !FMath::IsFinite(OutputArtifactSize)
				|| OutputArtifactSize < 1
				|| OutputArtifactSize
					> AkUGCInputBudget::MaxLogicPackBytes
				|| FMath::FloorToDouble(OutputArtifactSize)
					!= OutputArtifactSize)
			{
				return Fail(
					OutError,
					TEXT("Registry build 输出制品大小无效。"));
			}
			OutBuild.OutputArtifactSize =
				static_cast<int64>(OutputArtifactSize);
		}
		for (const FString* Timestamp : {
			&OutBuild.StartedAt,
			&OutBuild.FinishedAt})
		{
			if (!Timestamp->IsEmpty() && !IsTimestamp(*Timestamp))
			{
				return Fail(
					OutError,
					TEXT("Registry build 可选时间字段无效。"));
			}
		}

		const bool bHasOutputDigest =
			!OutBuild.OutputArtifactSha256.IsEmpty();
		const bool bHasOutputSize =
			Object->HasField(TEXT("outputArtifactSize"));
		const bool bHasError = !OutBuild.ErrorCode.IsEmpty();
		const bool bValidError =
			OutBuild.ErrorCode == TEXT("validation_rejected")
			|| OutBuild.ErrorCode == TEXT("validator_unavailable")
			|| OutBuild.ErrorCode == TEXT("artifact_unavailable")
			|| OutBuild.ErrorCode == TEXT("artifact_corrupt")
			|| OutBuild.ErrorCode == TEXT("lease_expired");
		if ((bHasOutputDigest
				&& !IsLowerHex(OutBuild.OutputArtifactSha256, 64))
			|| (OutBuild.Status == TEXT("queued")
				&& (OutBuild.Attempt != 0
					|| OutBuild.bRetryable
					|| bHasOutputDigest
					|| bHasOutputSize
					|| bHasError
					|| !OutBuild.StartedAt.IsEmpty()
					|| !OutBuild.FinishedAt.IsEmpty()))
			|| (OutBuild.Status == TEXT("running")
				&& (OutBuild.Attempt < 1
					|| OutBuild.bRetryable
					|| bHasOutputDigest
					|| bHasOutputSize
					|| bHasError
					|| OutBuild.StartedAt.IsEmpty()
					|| !OutBuild.FinishedAt.IsEmpty()))
			|| (OutBuild.Status == TEXT("succeeded")
				&& (OutBuild.Attempt < 1
					|| OutBuild.bRetryable
					|| OutBuild.OutputArtifactSha256
						!= ExpectedArtifactSha256
					|| !bHasOutputSize
					|| OutBuild.OutputArtifactSize
						!= ExpectedArtifactSize
					|| bHasError
					|| OutBuild.StartedAt.IsEmpty()
					|| OutBuild.FinishedAt.IsEmpty()))
			|| (OutBuild.Status == TEXT("failed")
				&& (OutBuild.Attempt < 1
					|| bHasOutputDigest
					|| bHasOutputSize
					|| !bHasError
					|| !bValidError
					|| OutBuild.StartedAt.IsEmpty()
					|| OutBuild.FinishedAt.IsEmpty())))
		{
			return Fail(
				OutError,
				TEXT("Registry build 状态与输出、错误或时间字段不一致。"));
		}
		return true;
	}
}

bool FAkUGCReleaseRegistryClient::ValidateSubmissionArtifact(
	const FAkUGCWorkEntry& Entry,
	const FString& SharePath,
	const FGuid& ExpectedProjectId,
	FString& OutError,
	FString* OutArtifactSha256,
	int64* OutArtifactSize)
{
	using namespace AkUGCReleaseRegistryClientPrivate;
	OutError.Reset();
	if (OutArtifactSha256)
	{
		OutArtifactSha256->Reset();
	}
	if (OutArtifactSize)
	{
		*OutArtifactSize = 0;
	}
	if (!ExpectedProjectId.IsValid()
		|| !Entry.ReleaseId.IsValid()
		|| !IsLowerHex(Entry.TrustedPublicKey, 64)
		|| !IsLowerHex(Entry.ContentHash, 64)
		|| !ValidateProjectDisplayName(Entry.DisplayName, OutError))
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("最近一次本地签名作品元数据无效。");
		}
		return false;
	}
	TArray<uint8> Artifact;
	if (!ReadArtifact(
		SharePath,
		Entry,
		ExpectedProjectId,
		Artifact,
		OutError))
	{
		return false;
	}
	if (OutArtifactSha256)
	{
		*OutArtifactSha256 =
			FAkUGCLogicPackHasher::Sha256Hex(Artifact);
	}
	if (OutArtifactSize)
	{
		*OutArtifactSize = Artifact.Num();
	}
	return true;
}

FAkUGCEditorHttpRequestPtr FAkUGCReleaseRegistryClient::StartSubmit(
	const FString& BaseUrl,
	const FString& BearerToken,
	const FAkUGCWorkEntry& Entry,
	const FString& SharePath,
	const FGuid& ExpectedProjectId,
	float TimeoutSeconds,
	FString& OutError)
{
	using namespace AkUGCReleaseRegistryClientPrivate;
	OutError.Reset();
	FString Normalized;
	if (!NormalizeBaseUrl(BaseUrl, Normalized, OutError)
		|| !ValidateBearerToken(BearerToken, OutError)
		|| !ExpectedProjectId.IsValid()
		|| !Entry.ReleaseId.IsValid()
		|| !IsLowerHex(Entry.TrustedPublicKey, 64)
		|| !IsLowerHex(Entry.ContentHash, 64))
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("最近一次本地签名作品元数据无效。");
		}
		return nullptr;
	}

	TArray<uint8> Artifact;
	if (!ReadArtifact(
			SharePath,
			Entry,
			ExpectedProjectId,
			Artifact,
			OutError))
	{
		return nullptr;
	}
	FAkUGCEditorHttpRequestSpec Spec;
	Spec.Method = EAkUGCEditorHttpMethod::Post;
	Spec.Url = Normalized + TEXT("/v1/releases");
	Spec.BearerToken = BearerToken;
	Spec.ContentType = TEXT("application/vnd.akugc+json");
	Spec.IdempotencyKey =
		TEXT("akugc-release-")
		+ Entry.ReleaseId.ToString(EGuidFormats::Digits);
	Spec.Body = MoveTemp(Artifact);
	Spec.TimeoutSeconds = TimeoutSeconds;
	Spec.MaxResponseBytes = MaxControlResponseBytes;
	Spec.bAllowLoopbackHttpWithBearer = true;
	FAkUGCEditorHttpRequestPtr Request =
		FAkUGCEditorHttpTransport::Start(MoveTemp(Spec));
	if (!Request.IsValid())
	{
		OutError = TEXT("Registry 传输不可用、忙碌或请求配置无效。");
	}
	return Request;
}

FAkUGCEditorHttpRequestPtr FAkUGCReleaseRegistryClient::StartGet(
	const FString& BaseUrl,
	const FString& BearerToken,
	const FGuid& ReleaseId,
	float TimeoutSeconds,
	FString& OutError)
{
	FAkUGCEditorHttpRequestPtr Request;
	AkUGCReleaseRegistryClientPrivate::StartCommon(
		BaseUrl,
		BearerToken,
		ReleaseId,
		EAkUGCEditorHttpMethod::Get,
		TEXT(""),
		TimeoutSeconds,
		Request,
		OutError);
	return Request;
}

FAkUGCEditorHttpRequestPtr FAkUGCReleaseRegistryClient::StartPublish(
	const FString& BaseUrl,
	const FString& BearerToken,
	const FGuid& ReleaseId,
	float TimeoutSeconds,
	FString& OutError)
{
	FAkUGCEditorHttpRequestPtr Request;
	AkUGCReleaseRegistryClientPrivate::StartCommon(
		BaseUrl,
		BearerToken,
		ReleaseId,
		EAkUGCEditorHttpMethod::Post,
		TEXT("/publish"),
		TimeoutSeconds,
		Request,
		OutError);
	return Request;
}

FAkUGCEditorHttpRequestPtr FAkUGCReleaseRegistryClient::StartGetBuild(
	const FString& BaseUrl,
	const FString& BearerToken,
	const FGuid& ReleaseId,
	float TimeoutSeconds,
	FString& OutError)
{
	FAkUGCEditorHttpRequestPtr Request;
	AkUGCReleaseRegistryClientPrivate::StartCommon(
		BaseUrl,
		BearerToken,
		ReleaseId,
		EAkUGCEditorHttpMethod::Get,
		TEXT("/build"),
		TimeoutSeconds,
		Request,
		OutError);
	return Request;
}

FAkUGCEditorHttpRequestPtr FAkUGCReleaseRegistryClient::StartRetryBuild(
	const FString& BaseUrl,
	const FString& BearerToken,
	const FGuid& ReleaseId,
	float TimeoutSeconds,
	FString& OutError)
{
	FAkUGCEditorHttpRequestPtr Request;
	AkUGCReleaseRegistryClientPrivate::StartCommon(
		BaseUrl,
		BearerToken,
		ReleaseId,
		EAkUGCEditorHttpMethod::Post,
		TEXT("/build/retry"),
		TimeoutSeconds,
		Request,
		OutError);
	return Request;
}

FAkUGCEditorHttpRequestPtr FAkUGCReleaseRegistryClient::StartCreateProject(
	const FString& BaseUrl,
	const FString& BearerToken,
	const FGuid& ProjectId,
	const FString& DisplayName,
	float TimeoutSeconds,
	FString& OutError)
{
	using namespace AkUGCReleaseRegistryClientPrivate;
	OutError.Reset();
	FString Normalized;
	if (!NormalizeBaseUrl(BaseUrl, Normalized, OutError)
		|| !ValidateBearerToken(BearerToken, OutError)
		|| !ProjectId.IsValid()
		|| !ValidateProjectDisplayName(DisplayName, OutError))
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("Project 创建参数无效。");
		}
		return nullptr;
	}

	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(
		TEXT("projectId"),
		ProjectId.ToString(EGuidFormats::DigitsWithHyphensLower));
	Object->SetStringField(TEXT("displayName"), DisplayName);
	TArray<uint8> Body;
	if (!SerializeJsonObject(Object, Body, OutError))
	{
		return nullptr;
	}

	FAkUGCEditorHttpRequestSpec Spec;
	Spec.Method = EAkUGCEditorHttpMethod::Post;
	Spec.Url = Normalized + TEXT("/v1/projects");
	Spec.BearerToken = BearerToken;
	Spec.ContentType = TEXT("application/json");
	Spec.IdempotencyKey =
		TEXT("akugc-project-") + ProjectId.ToString(EGuidFormats::Digits);
	Spec.Body = MoveTemp(Body);
	Spec.TimeoutSeconds = TimeoutSeconds;
	Spec.MaxResponseBytes = MaxControlResponseBytes;
	Spec.bAllowLoopbackHttpWithBearer = true;
	FAkUGCEditorHttpRequestPtr Request =
		FAkUGCEditorHttpTransport::Start(MoveTemp(Spec));
	if (!Request.IsValid())
	{
		OutError = TEXT("Registry 传输不可用、忙碌或请求配置无效。");
	}
	return Request;
}

FAkUGCEditorHttpRequestPtr FAkUGCReleaseRegistryClient::StartGetProject(
	const FString& BaseUrl,
	const FString& BearerToken,
	const FGuid& ProjectId,
	float TimeoutSeconds,
	FString& OutError)
{
	using namespace AkUGCReleaseRegistryClientPrivate;
	OutError.Reset();
	return StartProjectRequest(
		BaseUrl,
		BearerToken,
		ProjectId,
		EAkUGCEditorHttpMethod::Get,
		TEXT(""),
		{},
		TEXT(""),
		TEXT(""),
		false,
		TimeoutSeconds,
		OutError);
}

FAkUGCEditorHttpRequestPtr
FAkUGCReleaseRegistryClient::StartListProjectMembers(
	const FString& BaseUrl,
	const FString& BearerToken,
	const FGuid& ProjectId,
	float TimeoutSeconds,
	FString& OutError)
{
	using namespace AkUGCReleaseRegistryClientPrivate;
	OutError.Reset();
	return StartProjectRequest(
		BaseUrl,
		BearerToken,
		ProjectId,
		EAkUGCEditorHttpMethod::Get,
		TEXT("/members"),
		{},
		TEXT(""),
		TEXT(""),
		false,
		TimeoutSeconds,
		OutError);
}

FAkUGCEditorHttpRequestPtr FAkUGCReleaseRegistryClient::StartSetProjectMember(
	const FString& BaseUrl,
	const FString& BearerToken,
	const FGuid& ProjectId,
	const FString& Subject,
	const FString& Role,
	float TimeoutSeconds,
	FString& OutError)
{
	using namespace AkUGCReleaseRegistryClientPrivate;
	OutError.Reset();
	if (!ValidateMemberSubject(Subject, OutError)
		|| (Role != TEXT("editor") && Role != TEXT("viewer")))
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("协作者角色必须为 editor 或 viewer。");
		}
		return nullptr;
	}
	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("role"), Role);
	TArray<uint8> Body;
	if (!SerializeJsonObject(Object, Body, OutError))
	{
		return nullptr;
	}
	return StartProjectRequest(
		BaseUrl,
		BearerToken,
		ProjectId,
		EAkUGCEditorHttpMethod::Put,
		TEXT("/members/") + Subject,
		MoveTemp(Body),
		TEXT("application/json"),
		TEXT(""),
		false,
		TimeoutSeconds,
		OutError);
}

FAkUGCEditorHttpRequestPtr
FAkUGCReleaseRegistryClient::StartRemoveProjectMember(
	const FString& BaseUrl,
	const FString& BearerToken,
	const FGuid& ProjectId,
	const FString& Subject,
	float TimeoutSeconds,
	FString& OutError)
{
	using namespace AkUGCReleaseRegistryClientPrivate;
	OutError.Reset();
	if (!ValidateMemberSubject(Subject, OutError))
	{
		return nullptr;
	}
	return StartProjectRequest(
		BaseUrl,
		BearerToken,
		ProjectId,
		EAkUGCEditorHttpMethod::Delete,
		TEXT("/members/") + Subject,
		{},
		TEXT(""),
		TEXT(""),
		false,
		TimeoutSeconds,
		OutError);
}

FAkUGCEditorHttpRequestPtr
FAkUGCReleaseRegistryClient::StartListProjectVersions(
	const FString& BaseUrl,
	const FString& BearerToken,
	const FGuid& ProjectId,
	int32 Limit,
	float TimeoutSeconds,
	FString& OutError)
{
	using namespace AkUGCReleaseRegistryClientPrivate;
	OutError.Reset();
	if (Limit < 1 || Limit > MaxVersionsPerPage)
	{
		OutError = TEXT("版本历史分页上限必须在 1 到 100 之间。");
		return nullptr;
	}
	return StartProjectRequest(
		BaseUrl,
		BearerToken,
		ProjectId,
		EAkUGCEditorHttpMethod::Get,
		FString::Printf(TEXT("/versions?limit=%d"), Limit),
		{},
		TEXT(""),
		TEXT(""),
		true,
		TimeoutSeconds,
		OutError);
}

FAkUGCEditorHttpRequestPtr
FAkUGCReleaseRegistryClient::StartSetProjectVisibility(
	const FString& BaseUrl,
	const FString& BearerToken,
	const FGuid& ProjectId,
	const FString& Visibility,
	float TimeoutSeconds,
	FString& OutError)
{
	using namespace AkUGCReleaseRegistryClientPrivate;
	OutError.Reset();
	if (Visibility != TEXT("private") && Visibility != TEXT("public"))
	{
		OutError = TEXT("Project 可见性必须为 private 或 public。");
		return nullptr;
	}
	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("visibility"), Visibility);
	TArray<uint8> Body;
	if (!SerializeJsonObject(Object, Body, OutError))
	{
		return nullptr;
	}
	return StartProjectRequest(
		BaseUrl,
		BearerToken,
		ProjectId,
		EAkUGCEditorHttpMethod::Put,
		TEXT("/visibility"),
		MoveTemp(Body),
		TEXT("application/json"),
		TEXT(""),
		false,
		TimeoutSeconds,
		OutError);
}

bool FAkUGCReleaseRegistryClient::ParseResponse(
	EAkUGCReleaseRegistryOperation Operation,
	const FAkUGCEditorHttpResult& Transport,
	const FAkUGCWorkEntry& ExpectedEntry,
	const FGuid& ExpectedProjectId,
	FAkUGCReleaseRegistryResponse& OutResponse,
	FString& OutError)
{
	using namespace AkUGCReleaseRegistryClientPrivate;
	OutResponse = {};
	OutError.Reset();
	if (Transport.bTooLarge)
	{
		return Fail(OutError, TEXT("Registry 响应超过 64 KiB，已在接收时中止。"));
	}
	if (Transport.bTimedOut)
	{
		return Fail(OutError, TEXT("Registry 请求超时。"));
	}
	if (Transport.bCancelled)
	{
		return Fail(
			OutError,
			TEXT("Registry 请求已取消；若请求已经到达服务端，操作可能已完成，请刷新状态确认。"));
	}
	if (Transport.NativeCode != 0)
	{
		return Fail(
			OutError,
			FString::Printf(
				TEXT("Registry 传输失败（传输码 %d）。"),
				Transport.NativeCode));
	}
	const bool bExpectedHttp =
		(Operation == EAkUGCReleaseRegistryOperation::Submit
			&& (Transport.StatusCode == 200 || Transport.StatusCode == 201))
		|| (Operation != EAkUGCReleaseRegistryOperation::Submit
			&& Transport.StatusCode == 200);
	if (!bExpectedHttp)
	{
		return Fail(
			OutError,
			FString::Printf(
				TEXT("Registry 请求被拒绝（HTTP %d）。"),
				Transport.StatusCode));
	}
	if (Transport.Body.IsEmpty()
		|| Transport.Body.Num() > MaxControlResponseBytes)
	{
		return Fail(OutError, TEXT("Registry 响应为空或超过控制面预算。"));
	}

	FString Json;
	FFileHelper::BufferToString(
		Json,
		Transport.Body.GetData(),
		Transport.Body.Num());
	FTCHARToUTF8 RoundTrip(*Json);
	if (RoundTrip.Length() != Transport.Body.Num()
		|| FMemory::Memcmp(
			RoundTrip.Get(),
			Transport.Body.GetData(),
			Transport.Body.Num()) != 0)
	{
		return Fail(
			OutError,
			TEXT("Registry 响应不是规范 UTF-8 文本。"));
	}
	if (!AkUGCInputBudget::ValidateJson(
			Json,
			MaxControlResponseBytes,
			TEXT("Registry response"),
			&OutError))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(
			TJsonReaderFactory<>::Create(Json),
			Root)
		|| !Root.IsValid())
	{
		return Fail(OutError, TEXT("Registry 响应不是 JSON 对象。"));
	}

	const TSharedPtr<FJsonObject>* ReleaseObject = nullptr;
	switch (Operation)
	{
	case EAkUGCReleaseRegistryOperation::Submit:
		if (!ValidateFields(
				Root,
				{TEXT("created"), TEXT("release")},
				{},
				TEXT("Registry submit response"),
				OutError)
			|| !Root->TryGetBoolField(TEXT("created"), OutResponse.bCreated)
			|| !Root->TryGetObjectField(TEXT("release"), ReleaseObject)
			|| OutResponse.bCreated != (Transport.StatusCode == 201))
		{
			return Fail(OutError, TEXT("Registry 提交响应字段无效。"));
		}
		break;
	case EAkUGCReleaseRegistryOperation::Get:
		if (!ValidateFields(
				Root,
				{TEXT("release")},
				{},
				TEXT("Registry get response"),
				OutError)
			|| !Root->TryGetObjectField(TEXT("release"), ReleaseObject))
		{
			return Fail(OutError, TEXT("Registry 状态响应字段无效。"));
		}
		break;
	case EAkUGCReleaseRegistryOperation::Publish:
		if (!ValidateFields(
				Root,
				{TEXT("changed"), TEXT("release")},
				{},
				TEXT("Registry publish response"),
				OutError)
			|| !Root->TryGetBoolField(TEXT("changed"), OutResponse.bChanged)
			|| !Root->TryGetObjectField(TEXT("release"), ReleaseObject))
		{
			return Fail(OutError, TEXT("Registry 发布响应字段无效。"));
		}
		break;
	default:
		return Fail(OutError, TEXT("Registry 操作类型无效。"));
	}
	if (!ReleaseObject
		|| !ParseRelease(
			*ReleaseObject,
			ExpectedEntry,
			ExpectedProjectId,
			OutResponse.Release,
			OutError))
	{
		return false;
	}
	if (Operation == EAkUGCReleaseRegistryOperation::Publish
		&& OutResponse.Release.Status != TEXT("published"))
	{
		return Fail(OutError, TEXT("Registry 发布响应未进入 published 状态。"));
	}
	return true;
}

bool FAkUGCReleaseRegistryClient::ParseBuildResponse(
	EAkUGCBuildRegistryOperation Operation,
	const FAkUGCEditorHttpResult& Transport,
	const FGuid& ExpectedReleaseId,
	const FGuid& ExpectedProjectId,
	const FString& ExpectedArtifactSha256,
	int64 ExpectedArtifactSize,
	FAkUGCBuildRegistryResponse& OutResponse,
	FString& OutError)
{
	using namespace AkUGCReleaseRegistryClientPrivate;
	OutResponse = {};
	OutError.Reset();
	if (!ExpectedReleaseId.IsValid()
		|| !ExpectedProjectId.IsValid()
		|| !IsLowerHex(ExpectedArtifactSha256, 64)
		|| ExpectedArtifactSize < 1
		|| ExpectedArtifactSize > AkUGCInputBudget::MaxLogicPackBytes)
	{
		return Fail(
			OutError,
			TEXT("Registry build 本地绑定上下文无效。"));
	}
	if (Transport.bTooLarge)
	{
		return Fail(
			OutError,
			TEXT("Registry build 响应超过 64 KiB，已在接收时中止。"));
	}
	if (Transport.bHeadersTooLarge)
	{
		return Fail(
			OutError,
			TEXT("Registry build 响应 Header 超过预算。"));
	}
	if (Transport.bTimedOut)
	{
		return Fail(OutError, TEXT("Registry build 请求超时。"));
	}
	if (Transport.bCancelled)
	{
		return Fail(
			OutError,
			TEXT("Registry build 请求已取消；服务端状态可能已变化，请刷新确认。"));
	}
	if (Transport.NativeCode != 0)
	{
		return Fail(
			OutError,
			FString::Printf(
				TEXT("Registry build 传输失败（传输码 %d）。"),
				Transport.NativeCode));
	}
	if (Transport.StatusCode != 200)
	{
		return Fail(
			OutError,
			FString::Printf(
				TEXT("Registry build 请求被拒绝（HTTP %d）。"),
				Transport.StatusCode));
	}
	if (Transport.Body.IsEmpty()
		|| Transport.Body.Num() > MaxControlResponseBytes)
	{
		return Fail(
			OutError,
			TEXT("Registry build 响应为空或超过控制面预算。"));
	}

	FString Json;
	FFileHelper::BufferToString(
		Json,
		Transport.Body.GetData(),
		Transport.Body.Num());
	FTCHARToUTF8 RoundTrip(*Json);
	if (RoundTrip.Length() != Transport.Body.Num()
		|| FMemory::Memcmp(
			RoundTrip.Get(),
			Transport.Body.GetData(),
			Transport.Body.Num()) != 0)
	{
		return Fail(
			OutError,
			TEXT("Registry build 响应不是规范 UTF-8 文本。"));
	}
	if (!AkUGCInputBudget::ValidateJson(
			Json,
			MaxControlResponseBytes,
			TEXT("Registry build response"),
			&OutError))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(
			TJsonReaderFactory<>::Create(Json),
			Root)
		|| !Root.IsValid())
	{
		return Fail(
			OutError,
			TEXT("Registry build 响应不是 JSON 对象。"));
	}

	const TSharedPtr<FJsonObject>* BuildObject = nullptr;
	switch (Operation)
	{
	case EAkUGCBuildRegistryOperation::Get:
		if (!ValidateFields(
				Root,
				{TEXT("build")},
				{},
				TEXT("Registry get build response"),
				OutError)
			|| !Root->TryGetObjectField(TEXT("build"), BuildObject))
		{
			return Fail(
				OutError,
				TEXT("Registry build 状态响应字段无效。"));
		}
		break;
	case EAkUGCBuildRegistryOperation::Retry:
		if (!ValidateFields(
				Root,
				{TEXT("changed"), TEXT("build")},
				{},
				TEXT("Registry retry build response"),
				OutError)
			|| !Root->TryGetBoolField(
				TEXT("changed"),
				OutResponse.bChanged)
			|| !Root->TryGetObjectField(TEXT("build"), BuildObject))
		{
			return Fail(
				OutError,
				TEXT("Registry build 重试响应字段无效。"));
		}
		break;
	default:
		return Fail(
			OutError,
			TEXT("Registry build 操作类型无效。"));
	}
	return BuildObject
		&& ParseBuild(
			*BuildObject,
			ExpectedReleaseId,
			ExpectedProjectId,
			ExpectedArtifactSha256,
			ExpectedArtifactSize,
			OutResponse.Build,
			OutError);
}

bool FAkUGCReleaseRegistryClient::ParseProjectResponse(
	EAkUGCProjectRegistryOperation Operation,
	const FAkUGCEditorHttpResult& Transport,
	const FGuid& ExpectedProjectId,
	FAkUGCProjectRegistryResponse& OutResponse,
	FString& OutError)
{
	using namespace AkUGCReleaseRegistryClientPrivate;
	OutResponse = {};
	OutError.Reset();
	if (!ExpectedProjectId.IsValid())
	{
		return Fail(OutError, TEXT("预期 Project ID 无效。"));
	}
	if (Transport.bTooLarge)
	{
		return Fail(OutError, TEXT("Registry 响应超过 64 KiB，已在接收时中止。"));
	}
	if (Transport.bTimedOut)
	{
		return Fail(OutError, TEXT("Registry 请求超时。"));
	}
	if (Transport.bCancelled)
	{
		return Fail(
			OutError,
			TEXT("Registry 请求已取消；若请求已经到达服务端，操作可能已完成，请刷新状态确认。"));
	}
	if (Transport.NativeCode != 0)
	{
		return Fail(
			OutError,
			FString::Printf(
				TEXT("Registry 传输失败（传输码 %d）。"),
				Transport.NativeCode));
	}
	const bool bExpectedHttp =
		(Operation == EAkUGCProjectRegistryOperation::CreateProject
			&& (Transport.StatusCode == 200 || Transport.StatusCode == 201))
		|| (Operation != EAkUGCProjectRegistryOperation::CreateProject
			&& Transport.StatusCode == 200);
	if (!bExpectedHttp)
	{
		return Fail(
			OutError,
			FString::Printf(
				TEXT("Registry 请求被拒绝（HTTP %d）。"),
				Transport.StatusCode));
	}
	if (Transport.Body.IsEmpty()
		|| Transport.Body.Num() > MaxControlResponseBytes)
	{
		return Fail(OutError, TEXT("Registry 响应为空或超过控制面预算。"));
	}

	FString Json;
	FFileHelper::BufferToString(
		Json,
		Transport.Body.GetData(),
		Transport.Body.Num());
	FTCHARToUTF8 RoundTrip(*Json);
	if (RoundTrip.Length() != Transport.Body.Num()
		|| FMemory::Memcmp(
			RoundTrip.Get(),
			Transport.Body.GetData(),
			Transport.Body.Num()) != 0)
	{
		return Fail(OutError, TEXT("Registry 响应不是规范 UTF-8 文本。"));
	}
	if (!AkUGCInputBudget::ValidateJson(
			Json,
			MaxControlResponseBytes,
			TEXT("Registry project response"),
			&OutError))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(
			TJsonReaderFactory<>::Create(Json),
			Root)
		|| !Root.IsValid())
	{
		return Fail(OutError, TEXT("Registry 响应不是 JSON 对象。"));
	}

	const TSharedPtr<FJsonObject>* ProjectObject = nullptr;
	const TSharedPtr<FJsonObject>* MemberObject = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	switch (Operation)
	{
	case EAkUGCProjectRegistryOperation::CreateProject:
		if (!ValidateFields(
				Root,
				{TEXT("created"), TEXT("project")},
				{},
				TEXT("Registry create project response"),
				OutError)
			|| !Root->TryGetBoolField(TEXT("created"), OutResponse.bCreated)
			|| !Root->TryGetObjectField(TEXT("project"), ProjectObject)
			|| OutResponse.bCreated != (Transport.StatusCode == 201)
			|| !ProjectObject
			|| !ParseProject(
				*ProjectObject,
				ExpectedProjectId,
				OutResponse.Project,
				OutError))
		{
			return Fail(OutError, TEXT("Registry 创建 Project 响应字段无效。"));
		}
		OutResponse.AccessRole = TEXT("owner");
		break;
	case EAkUGCProjectRegistryOperation::GetProject:
		if (!ValidateFields(
				Root,
				{TEXT("accessRole"), TEXT("project")},
				{},
				TEXT("Registry get project response"),
				OutError)
			|| !Root->TryGetStringField(
				TEXT("accessRole"),
				OutResponse.AccessRole)
			|| !IsProjectRole(OutResponse.AccessRole)
			|| !Root->TryGetObjectField(TEXT("project"), ProjectObject)
			|| !ProjectObject
			|| !ParseProject(
				*ProjectObject,
				ExpectedProjectId,
				OutResponse.Project,
				OutError))
		{
			return Fail(OutError, TEXT("Registry Project 状态响应字段无效。"));
		}
		break;
	case EAkUGCProjectRegistryOperation::ListMembers:
		if (!ValidateFields(
				Root,
				{TEXT("members")},
				{},
				TEXT("Registry member list response"),
				OutError)
			|| !Root->TryGetArrayField(TEXT("members"), Values)
			|| !Values
			|| Values->Num() > MaxProjectMembers)
		{
			return Fail(OutError, TEXT("Registry 协作者列表响应字段无效。"));
		}
		{
			TSet<FString> Subjects;
			int32 OwnerCount = 0;
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				const TSharedPtr<FJsonObject>* Object = nullptr;
				if (!Value.IsValid()
					|| !Value->TryGetObject(Object)
					|| !Object
					|| !ParseMember(
						*Object,
						ExpectedProjectId,
						OutResponse.Members.Emplace_GetRef(),
						OutError))
				{
					return false;
				}
				const FString& Subject = OutResponse.Members.Last().Subject;
				if (Subjects.Contains(Subject))
				{
					return Fail(
						OutError,
						TEXT("Registry 协作者列表包含重复 subject。"));
				}
				Subjects.Add(Subject);
				if (OutResponse.Members.Last().Role == TEXT("owner"))
				{
					++OwnerCount;
				}
			}
			if (OwnerCount != 1)
			{
				return Fail(
					OutError,
					TEXT("Registry 协作者列表必须且只能包含一个 Owner。"));
			}
		}
		return true;
	case EAkUGCProjectRegistryOperation::SetMember:
		if (!ValidateFields(
				Root,
				{TEXT("changed"), TEXT("member"), TEXT("project")},
				{},
				TEXT("Registry set member response"),
				OutError)
			|| !Root->TryGetBoolField(TEXT("changed"), OutResponse.bChanged)
			|| !Root->TryGetObjectField(TEXT("member"), MemberObject)
			|| !Root->TryGetObjectField(TEXT("project"), ProjectObject)
			|| !MemberObject
			|| !ProjectObject
			|| !ParseMember(
				*MemberObject,
				ExpectedProjectId,
				OutResponse.Member,
				OutError)
			|| !ParseProject(
				*ProjectObject,
				ExpectedProjectId,
				OutResponse.Project,
				OutError))
		{
			return Fail(OutError, TEXT("Registry 设置协作者响应字段无效。"));
		}
		OutResponse.AccessRole = TEXT("owner");
		break;
	case EAkUGCProjectRegistryOperation::RemoveMember:
		if (!ValidateFields(
				Root,
				{TEXT("changed"), TEXT("project")},
				{},
				TEXT("Registry remove member response"),
				OutError)
			|| !Root->TryGetBoolField(TEXT("changed"), OutResponse.bChanged)
			|| !Root->TryGetObjectField(TEXT("project"), ProjectObject)
			|| !ProjectObject
			|| !ParseProject(
				*ProjectObject,
				ExpectedProjectId,
				OutResponse.Project,
				OutError))
		{
			return Fail(OutError, TEXT("Registry 移除协作者响应字段无效。"));
		}
		OutResponse.AccessRole = TEXT("owner");
		break;
	case EAkUGCProjectRegistryOperation::SetVisibility:
		if (!ValidateFields(
				Root,
				{TEXT("changed"), TEXT("project")},
				{},
				TEXT("Registry set visibility response"),
				OutError)
			|| !Root->TryGetBoolField(TEXT("changed"), OutResponse.bChanged)
			|| !Root->TryGetObjectField(TEXT("project"), ProjectObject)
			|| !ProjectObject
			|| !ParseProject(
				*ProjectObject,
				ExpectedProjectId,
				OutResponse.Project,
				OutError))
		{
			return Fail(
				OutError,
				TEXT("Registry 设置 Project 可见性响应字段无效。"));
		}
		OutResponse.AccessRole = TEXT("owner");
		break;
	case EAkUGCProjectRegistryOperation::ListVersions:
	{
		if (!ValidateFields(
				Root,
				{TEXT("versions")},
				{TEXT("nextCursor")},
				TEXT("Registry version list response"),
				OutError)
			|| !Root->TryGetArrayField(TEXT("versions"), Values)
			|| !Values
			|| Values->Num() > MaxVersionsPerPage)
		{
			return Fail(OutError, TEXT("Registry 版本历史响应字段无效。"));
		}
		FGuid Cursor;
		if (Root->HasField(TEXT("nextCursor")))
		{
			if (!Root->TryGetStringField(
					TEXT("nextCursor"),
					OutResponse.NextCursor)
				|| !ParseCanonicalGuid(OutResponse.NextCursor, Cursor))
			{
				return Fail(
					OutError,
					TEXT("Registry 版本历史游标无效。"));
			}
		}
		{
			TSet<FGuid> ReleaseIds;
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				const TSharedPtr<FJsonObject>* Object = nullptr;
				if (!Value.IsValid()
					|| !Value->TryGetObject(Object)
					|| !Object
					|| !ParseReleaseMetadata(
						*Object,
						ExpectedProjectId,
						OutResponse.Versions.Emplace_GetRef(),
						OutError))
				{
					return false;
				}
				const FGuid ReleaseId =
					OutResponse.Versions.Last().ReleaseId;
				if (ReleaseIds.Contains(ReleaseId))
				{
					return Fail(
						OutError,
						TEXT("Registry 版本历史包含重复 Release ID。"));
				}
				ReleaseIds.Add(ReleaseId);
			}
		}
		if (!OutResponse.NextCursor.IsEmpty()
			&& (OutResponse.Versions.IsEmpty()
				|| OutResponse.Versions.Last().ReleaseId != Cursor))
		{
			return Fail(
				OutError,
				TEXT("Registry 版本历史游标未绑定当前页最后一个版本。"));
		}
		return true;
	}
	default:
		return Fail(OutError, TEXT("Registry Project 操作类型无效。"));
	}
	if (OutResponse.Project.ProjectId != ExpectedProjectId)
	{
		return Fail(OutError, TEXT("Registry Project ID 与当前文档不一致。"));
	}
	return true;
}

bool FAkUGCReleaseRegistryClient::NormalizeBaseUrl(
	const FString& BaseUrl,
	FString& OutNormalized,
	FString& OutError)
{
	using namespace AkUGCReleaseRegistryClientPrivate;
	OutNormalized.Reset();
	OutError.Reset();
	FString Candidate = BaseUrl.TrimStartAndEnd();
	if (Candidate.EndsWith(TEXT("/")))
	{
		Candidate.LeftChopInline(1);
	}
	if (!Candidate.StartsWith(TEXT("http://"), ESearchCase::CaseSensitive)
		|| Candidate.Len() > AkUGCInputBudget::MaxCopilotUrlCharacters)
	{
		return Fail(
			OutError,
			TEXT("P19 本机 Registry 地址必须使用 http:// loopback。"));
	}
	for (const TCHAR Character : Candidate)
	{
		if (Character <= 32
			|| Character >= 127
			|| Character == TEXT('@')
			|| Character == TEXT('\\')
			|| Character == TEXT('#')
			|| Character == TEXT('?'))
		{
			return Fail(OutError, TEXT("Registry 地址包含禁止字符。"));
		}
	}
	const FString Authority = Candidate.Mid(7);
	if (Authority.IsEmpty() || Authority.Contains(TEXT("/")))
	{
		return Fail(OutError, TEXT("Registry 地址不能包含路径。"));
	}
	FString Host = Authority;
	int32 Colon = INDEX_NONE;
	if (Authority.FindChar(TEXT(':'), Colon))
	{
		Host = Authority.Left(Colon);
		const FString Port = Authority.Mid(Colon + 1);
		if (Port.IsEmpty() || Port.Len() > 5)
		{
			return Fail(OutError, TEXT("Registry 端口无效。"));
		}
		for (const TCHAR Character : Port)
		{
			if (Character < TEXT('0') || Character > TEXT('9'))
			{
				return Fail(OutError, TEXT("Registry 端口无效。"));
			}
		}
		const int32 Number = FCString::Atoi(*Port);
		if (Number < 1 || Number > 65535)
		{
			return Fail(OutError, TEXT("Registry 端口无效。"));
		}
	}
	if (!Host.Equals(TEXT("localhost"), ESearchCase::IgnoreCase)
		&& Host != TEXT("127.0.0.1"))
	{
		return Fail(
			OutError,
			TEXT("P19 静态 token 只允许发送到 localhost 或 127.0.0.1。"));
	}
	OutNormalized = Candidate;
	return true;
}

bool FAkUGCReleaseRegistryClient::ValidateBearerToken(
	const FString& BearerToken,
	FString& OutError)
{
	using namespace AkUGCReleaseRegistryClientPrivate;
	if (BearerToken.IsEmpty()
		|| BearerToken.Len() > AkUGCInputBudget::MaxCopilotApiKeyBytes)
	{
		return Fail(OutError, TEXT("Creator token 为空或超过 4096 字节。"));
	}
	for (const TCHAR Character : BearerToken)
	{
		if (Character <= 32 || Character >= 127)
		{
			return Fail(
				OutError,
				TEXT("Creator token 只能包含可见 ASCII 字符。"));
		}
	}
	return true;
}

bool FAkUGCReleaseRegistryClient::ValidateProjectDisplayName(
	const FString& DisplayName,
	FString& OutError)
{
	using namespace AkUGCReleaseRegistryClientPrivate;
	if (DisplayName.IsEmpty()
		|| DisplayName != DisplayName.TrimStartAndEnd()
		|| FTCHARToUTF8(*DisplayName).Length() > 128)
	{
		return Fail(
			OutError,
			TEXT("Project 名称必须为 1 到 128 字节且首尾无空白。"));
	}
	for (const TCHAR Character : DisplayName)
	{
		if (Character < 32 || Character == 127)
		{
			return Fail(OutError, TEXT("Project 名称包含控制字符。"));
		}
	}
	return true;
}

bool FAkUGCReleaseRegistryClient::ValidateMemberSubject(
	const FString& Subject,
	FString& OutError)
{
	using namespace AkUGCReleaseRegistryClientPrivate;
	if (!IsSafeSubject(Subject))
	{
		return Fail(
			OutError,
			TEXT("协作者 subject 必须为 1 到 128 个安全 ASCII 字符。"));
	}
	return true;
}

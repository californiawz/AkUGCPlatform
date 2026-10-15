#include "Registry/AkUGCReleaseRegistryClient.h"

#include <initializer_list>

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Pack/AkUGCLogicPackLoader.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Validation/AkUGCInputBudget.h"

namespace AkUGCReleaseRegistryClientPrivate
{
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
		const FAkUGCLogicPackLoadResult Verified =
			FAkUGCLogicPackLoader::LoadVerified(Json, Entry.TrustedPublicKey);
		if (!Verified.bSucceeded
			|| Verified.Pack.Manifest.ReleaseId != Entry.ReleaseId
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

	bool ParseRelease(
		const TSharedPtr<FJsonObject>& Object,
		const FAkUGCWorkEntry& Expected,
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
		FString Owner;
		FString SubmittedBy;
		FString PublicKey;
		FString ContentHash;
		FString ArtifactSha256;
		FString Status;
		FString SubmittedAt;
		FString UpdatedAt;
		double SchemaVersion = -1;
		double ArtifactSize = -1;
		double Revision = -1;
		if (!Object->TryGetStringField(TEXT("releaseId"), ReleaseId)
			|| !Object->TryGetStringField(TEXT("projectId"), ProjectId)
			|| !Object->TryGetNumberField(TEXT("schemaVersion"), SchemaVersion)
			|| !Object->TryGetStringField(TEXT("ownerSubject"), Owner)
			|| !Object->TryGetStringField(TEXT("submittedBySubject"), SubmittedBy)
			|| !Object->TryGetStringField(TEXT("publisherPublicKey"), PublicKey)
			|| !Object->TryGetStringField(TEXT("contentHash"), ContentHash)
			|| !Object->TryGetStringField(TEXT("artifactSha256"), ArtifactSha256)
			|| !Object->TryGetNumberField(TEXT("artifactSize"), ArtifactSize)
			|| !Object->TryGetStringField(TEXT("status"), Status)
			|| !Object->TryGetNumberField(TEXT("revision"), Revision)
			|| !Object->TryGetStringField(TEXT("submittedAt"), SubmittedAt)
			|| !Object->TryGetStringField(TEXT("updatedAt"), UpdatedAt))
		{
			return Fail(OutError, TEXT("Registry release 字段类型无效。"));
		}
		for (const TCHAR* Name : {
			TEXT("reviewedAt"),
			TEXT("publishedAt"),
			TEXT("revokedAt")})
		{
			FString Value;
			if (Object->HasField(Name)
				&& (!Object->TryGetStringField(Name, Value)
					|| Value.IsEmpty()
					|| Value.Len() > 64))
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
		const FString ExpectedId =
			Expected.ReleaseId.ToString(EGuidFormats::DigitsWithHyphensLower);
		FGuid ParsedProjectId;
		if (ReleaseId != ExpectedId
			|| !FGuid::Parse(ProjectId, ParsedProjectId)
			|| !ParsedProjectId.IsValid()
			|| Owner.IsEmpty()
			|| Owner.Len() > 128
			|| SubmittedBy.IsEmpty()
			|| SubmittedBy.Len() > 128
			|| PublicKey != Expected.TrustedPublicKey
			|| ContentHash != Expected.ContentHash
			|| !IsLowerHex(PublicKey, 64)
			|| !IsLowerHex(ContentHash, 64)
			|| !IsLowerHex(ArtifactSha256, 64)
			|| !FMath::IsFinite(SchemaVersion)
			|| SchemaVersion < 0
			|| FMath::FloorToDouble(SchemaVersion) != SchemaVersion
			|| !FMath::IsFinite(ArtifactSize)
			|| ArtifactSize < 1
			|| ArtifactSize > AkUGCInputBudget::MaxLogicPackBytes
			|| FMath::FloorToDouble(ArtifactSize) != ArtifactSize
			|| !FMath::IsFinite(Revision)
			|| Revision < 1
			|| FMath::FloorToDouble(Revision) != Revision
			|| SubmittedAt.IsEmpty()
			|| SubmittedAt.Len() > 64
			|| UpdatedAt.IsEmpty()
			|| UpdatedAt.Len() > 64
			|| (Status != TEXT("submitted")
				&& Status != TEXT("approved")
				&& Status != TEXT("rejected")
				&& Status != TEXT("published")
				&& Status != TEXT("revoked")))
		{
			return Fail(
				OutError,
				TEXT("Registry release 与本地签名作品不一致或字段越界。"));
		}

		OutRelease.ReleaseId = Expected.ReleaseId;
		OutRelease.Status = Status;
		OutRelease.OwnerSubject = Owner;
		OutRelease.SubmittedBySubject = SubmittedBy;
		OutRelease.PublisherPublicKey = PublicKey;
		OutRelease.ContentHash = ContentHash;
		OutRelease.ArtifactSha256 = ArtifactSha256;
		OutRelease.Revision = static_cast<int64>(Revision);
		return true;
	}
}

FAkUGCEditorHttpRequestPtr FAkUGCReleaseRegistryClient::StartSubmit(
	const FString& BaseUrl,
	const FString& BearerToken,
	const FAkUGCWorkEntry& Entry,
	const FString& SharePath,
	float TimeoutSeconds,
	FString& OutError)
{
	using namespace AkUGCReleaseRegistryClientPrivate;
	OutError.Reset();
	FString Normalized;
	if (!NormalizeBaseUrl(BaseUrl, Normalized, OutError)
		|| !ValidateBearerToken(BearerToken, OutError)
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
	if (!ReadArtifact(SharePath, Entry, Artifact, OutError))
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

bool FAkUGCReleaseRegistryClient::ParseResponse(
	EAkUGCReleaseRegistryOperation Operation,
	const FAkUGCEditorHttpResult& Transport,
	const FAkUGCWorkEntry& ExpectedEntry,
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

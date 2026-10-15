#include "Release/AkUGCPlayerReleaseRegistryClient.h"

#include <initializer_list>

#include "Dom/JsonObject.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Pack/AkUGCLogicPackHasher.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Validation/AkUGCInputBudget.h"

namespace AkUGCPlayerRegistryPrivate
{
	constexpr double MaxJsonSafeInteger = 9007199254740991.0;

	bool Fail(
		FString& OutFailureCode,
		FString& OutError,
		const TCHAR* Code,
		const FString& Message)
	{
		OutFailureCode = Code;
		OutError = Message;
		return false;
	}

	bool FailConfiguration(FString& OutError, const FString& Message)
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
		FString& OutFailureCode,
		FString& OutError)
	{
		if (!Object.IsValid())
		{
			return Fail(
				OutFailureCode,
				OutError,
				TEXT("protocol"),
				FString::Printf(TEXT("%s 不是 JSON 对象。"), Label));
		}
		TSet<FString> Allowed;
		for (const TCHAR* Name : Required)
		{
			Allowed.Add(Name);
			if (!Object->HasField(Name))
			{
				return Fail(
					OutFailureCode,
					OutError,
					TEXT("protocol"),
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
					OutFailureCode,
					OutError,
					TEXT("protocol"),
					FString::Printf(TEXT("%s 包含未知字段。"), Label));
			}
		}
		return true;
	}

	bool ParseCanonicalGuid(const FString& Value, FGuid& OutGuid)
	{
		return FGuid::Parse(Value, OutGuid)
			&& OutGuid.IsValid()
			&& Value
				== OutGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
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

	bool IsTimestamp(const FString& Value)
	{
		FDateTime Parsed;
		return Value.Len() >= 20
			&& Value.Len() <= 64
			&& FDateTime::ParseIso8601(*Value, Parsed);
	}

	bool FindSingleHeader(
		const FAkUGCHttpResult& Transport,
		const TCHAR* Name,
		FString& OutValue,
		FString& OutFailureCode,
		FString& OutError)
	{
		OutValue.Reset();
		int32 Matches = 0;
		for (const FAkUGCHttpHeader& Header : Transport.Headers)
		{
			if (Header.Name.Equals(Name, ESearchCase::IgnoreCase))
			{
				++Matches;
				OutValue = Header.Value;
			}
		}
		if (Matches != 1)
		{
			return Fail(
				OutFailureCode,
				OutError,
				TEXT("protocol"),
				FString::Printf(
					TEXT("Registry 响应必须且只能包含一个 %s Header。"),
					Name));
		}
		return true;
	}

	bool HasHeader(
		const FAkUGCHttpResult& Transport,
		const TCHAR* Name)
	{
		return Transport.Headers.ContainsByPredicate(
			[Name](const FAkUGCHttpHeader& Header)
			{
				return Header.Name.Equals(
					Name,
					ESearchCase::IgnoreCase);
			});
	}

	bool ParseDecimal(const FString& Value, int64& OutValue)
	{
		if (Value.IsEmpty())
		{
			return false;
		}
		uint64 Parsed = 0;
		for (const TCHAR Character : Value)
		{
			if (Character < TEXT('0') || Character > TEXT('9'))
			{
				return false;
			}
			const uint64 Digit =
				static_cast<uint64>(Character - TEXT('0'));
			if (Parsed
				> (static_cast<uint64>(MAX_int64) - Digit) / 10ULL)
			{
				return false;
			}
			Parsed = Parsed * 10ULL + Digit;
		}
		OutValue = static_cast<int64>(Parsed);
		return true;
	}

	bool IsDisplayName(const FString& Value)
	{
		if (Value.IsEmpty()
			|| Value != Value.TrimStartAndEnd()
			|| FTCHARToUTF8(*Value).Length() > 128)
		{
			return false;
		}
		for (const TCHAR Character : Value)
		{
			if (Character < 32 || Character == 127)
			{
				return false;
			}
		}
		return true;
	}

	bool ParsePublishedRelease(
		const TSharedPtr<FJsonObject>& Object,
		const FGuid& ExpectedProjectId,
		FGuid& OutReleaseId,
		FString& OutFailureCode,
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
				TEXT("Registry work latestRelease"),
				OutFailureCode,
				OutError))
		{
			return false;
		}
		FString ReleaseId;
		FString ProjectId;
		FString OwnerSubject;
		FString SubmittedBySubject;
		FString PublisherPublicKey;
		FString ContentHash;
		FString ArtifactSha256;
		FString Status;
		FString SubmittedAt;
		FString UpdatedAt;
		double SchemaVersion = -1;
		double ArtifactSize = -1;
		double Revision = -1;
		FGuid ParsedProjectId;
		if (!Object->TryGetStringField(TEXT("releaseId"), ReleaseId)
			|| !Object->TryGetStringField(TEXT("projectId"), ProjectId)
			|| !Object->TryGetNumberField(TEXT("schemaVersion"), SchemaVersion)
			|| !Object->TryGetStringField(TEXT("ownerSubject"), OwnerSubject)
			|| !Object->TryGetStringField(
				TEXT("submittedBySubject"),
				SubmittedBySubject)
			|| !Object->TryGetStringField(
				TEXT("publisherPublicKey"),
				PublisherPublicKey)
			|| !Object->TryGetStringField(TEXT("contentHash"), ContentHash)
			|| !Object->TryGetStringField(
				TEXT("artifactSha256"),
				ArtifactSha256)
			|| !Object->TryGetNumberField(TEXT("artifactSize"), ArtifactSize)
			|| !Object->TryGetStringField(TEXT("status"), Status)
			|| !Object->TryGetNumberField(TEXT("revision"), Revision)
			|| !Object->TryGetStringField(TEXT("submittedAt"), SubmittedAt)
			|| !Object->TryGetStringField(TEXT("updatedAt"), UpdatedAt)
			|| !ParseCanonicalGuid(ReleaseId, OutReleaseId)
			|| !ParseCanonicalGuid(ProjectId, ParsedProjectId)
			|| ParsedProjectId != ExpectedProjectId
			|| !IsSafeSubject(OwnerSubject)
			|| !IsSafeSubject(SubmittedBySubject)
			|| !IsLowerHex(PublisherPublicKey, 64)
			|| !IsLowerHex(ContentHash, 64)
			|| !IsLowerHex(ArtifactSha256, 64)
			|| Status != TEXT("published")
			|| !FMath::IsFinite(SchemaVersion)
			|| SchemaVersion < 0
			|| SchemaVersion > MAX_int32
			|| FMath::FloorToDouble(SchemaVersion) != SchemaVersion
			|| !FMath::IsFinite(ArtifactSize)
			|| ArtifactSize < 1
			|| ArtifactSize
				> FAkUGCPlayerReleaseRegistryClient::MaxArtifactBytes
			|| FMath::FloorToDouble(ArtifactSize) != ArtifactSize
			|| !FMath::IsFinite(Revision)
			|| Revision < 1
			|| Revision > MaxJsonSafeInteger
			|| FMath::FloorToDouble(Revision) != Revision
			|| !IsTimestamp(SubmittedAt)
			|| !IsTimestamp(UpdatedAt)
			|| !Object->HasField(TEXT("publishedAt")))
		{
			return Fail(
				OutFailureCode,
				OutError,
				TEXT("protocol"),
				TEXT("Registry work latestRelease 字段无效或与请求 Project 不一致。"));
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
					OutFailureCode,
					OutError,
					TEXT("protocol"),
					TEXT("Registry work latestRelease 时间字段无效。"));
			}
		}
		FString ReviewReason;
		if (Object->HasField(TEXT("reviewReason"))
			&& (!Object->TryGetStringField(TEXT("reviewReason"), ReviewReason)
				|| FTCHARToUTF8(*ReviewReason).Length() > 1024))
		{
			return Fail(
				OutFailureCode,
				OutError,
				TEXT("protocol"),
				TEXT("Registry work latestRelease 审核原因无效。"));
		}
		return true;
	}
}

FAkUGCHttpRequestPtr
FAkUGCPlayerReleaseRegistryClient::StartGetWorkPage(
	const FString& BaseUrl,
	const FString& BearerToken,
	const FGuid& ProjectId,
	float TimeoutSeconds,
	FString& OutExpectedUrl,
	FString& OutError)
{
	OutExpectedUrl.Reset();
	OutError.Reset();
	FString Normalized;
	if (!NormalizeBaseUrl(BaseUrl, Normalized, OutError)
		|| !ValidateBearerToken(BearerToken, OutError))
	{
		return nullptr;
	}
	if (!ProjectId.IsValid())
	{
		OutError = TEXT("Project ID 无效。");
		return nullptr;
	}
	OutExpectedUrl =
		Normalized
		+ TEXT("/v1/works/")
		+ ProjectId.ToString(EGuidFormats::DigitsWithHyphensLower);

	FAkUGCHttpRequestSpec Spec;
	Spec.Method = EAkUGCHttpMethod::Get;
	Spec.Url = OutExpectedUrl;
	Spec.BearerToken = BearerToken;
	Spec.TimeoutSeconds = TimeoutSeconds;
	Spec.MaxResponseBytes = MaxResponseHeaderBytes;
	Spec.MaxResponseHeaderBytes = MaxResponseHeaderBytes;
	Spec.bAllowLoopbackHttpWithBearer = true;
	Spec.bAcceptCompressedResponse = false;
	FAkUGCHttpRequestPtr Request =
		FAkUGCHttpTransport::Start(MoveTemp(Spec));
	if (!Request.IsValid())
	{
		OutError =
			TEXT("Registry 传输不可用、忙碌或请求配置无效。");
		OutExpectedUrl.Reset();
	}
	return Request;
}

bool FAkUGCPlayerReleaseRegistryClient::ParseWorkPage(
	const FAkUGCHttpResult& Transport,
	const FString& ExpectedUrl,
	const FGuid& ExpectedProjectId,
	FAkUGCPlayerRegistryWorkPage& OutWork,
	FString& OutFailureCode,
	FString& OutError)
{
	using namespace AkUGCPlayerRegistryPrivate;
	OutWork = {};
	OutFailureCode.Reset();
	OutError.Reset();
	if (ExpectedUrl.IsEmpty() || !ExpectedProjectId.IsValid())
	{
		return Fail(
			OutFailureCode,
			OutError,
			TEXT("invalid_configuration"),
			TEXT("Player Registry 作品页上下文无效。"));
	}
	if (Transport.bTooLarge)
	{
		return Fail(
			OutFailureCode,
			OutError,
			TEXT("response_too_large"),
			TEXT("Registry 作品页响应超过 64 KiB，已中止。"));
	}
	if (Transport.bHeadersTooLarge)
	{
		return Fail(
			OutFailureCode,
			OutError,
			TEXT("header_too_large"),
			TEXT("Registry 响应 Header 超过 64 KiB，已中止。"));
	}
	if (Transport.bTimedOut)
	{
		return Fail(
			OutFailureCode,
			OutError,
			TEXT("timeout"),
			TEXT("Registry 作品页请求超时。"));
	}
	if (Transport.bCancelled)
	{
		return Fail(
			OutFailureCode,
			OutError,
			TEXT("cancelled"),
			TEXT("Registry 作品页请求已取消。"));
	}
	if (Transport.NativeCode != 0)
	{
		return Fail(
			OutFailureCode,
			OutError,
			TEXT("transport"),
			FString::Printf(
				TEXT("Registry 作品页传输失败（传输码 %d）。"),
				Transport.NativeCode));
	}
	if (Transport.EffectiveUrl != ExpectedUrl
		|| (Transport.StatusCode >= 300 && Transport.StatusCode < 400))
	{
		return Fail(
			OutFailureCode,
			OutError,
			TEXT("redirect"),
			TEXT("Registry 作品页拒绝重定向或最终地址变化。"));
	}
	if (Transport.StatusCode != 200)
	{
		const TCHAR* Code = TEXT("http_status");
		if (Transport.StatusCode == 401)
		{
			Code = TEXT("unauthorized");
		}
		else if (Transport.StatusCode == 403)
		{
			Code = TEXT("forbidden");
		}
		else if (Transport.StatusCode == 404)
		{
			Code = TEXT("not_available");
		}
		return Fail(
			OutFailureCode,
			OutError,
			Code,
			FString::Printf(
				TEXT("Registry 作品页请求被拒绝（HTTP %d）。"),
				Transport.StatusCode));
	}
	if (Transport.Body.IsEmpty()
		|| Transport.Body.Num() > MaxResponseHeaderBytes
		|| HasHeader(Transport, TEXT("Location"))
		|| HasHeader(Transport, TEXT("Content-Encoding")))
	{
		return Fail(
			OutFailureCode,
			OutError,
			TEXT("protocol"),
			TEXT("Registry 作品页响应为空、超限或包含禁止 Header。"));
	}
	FString ContentType;
	if (!FindSingleHeader(
			Transport,
			TEXT("Content-Type"),
			ContentType,
			OutFailureCode,
			OutError)
		|| ContentType != TEXT("application/json; charset=utf-8"))
	{
		if (OutFailureCode.IsEmpty())
		{
			return Fail(
				OutFailureCode,
				OutError,
				TEXT("protocol"),
				TEXT("Registry 作品页 Content-Type 无效。"));
		}
		return false;
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
			OutFailureCode,
			OutError,
			TEXT("protocol"),
			TEXT("Registry 作品页响应不是规范 UTF-8 文本。"));
	}
	if (!AkUGCInputBudget::ValidateJson(
			Json,
			MaxResponseHeaderBytes,
			TEXT("Registry work response"),
			&OutError))
	{
		OutFailureCode = TEXT("protocol");
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(
			TJsonReaderFactory<>::Create(Json),
			Root)
		|| !Root.IsValid()
		|| !ValidateFields(
			Root,
			{TEXT("work")},
			{},
			TEXT("Registry work response"),
			OutFailureCode,
			OutError))
	{
		if (OutFailureCode.IsEmpty())
		{
			return Fail(
				OutFailureCode,
				OutError,
				TEXT("protocol"),
				TEXT("Registry 作品页响应不是有效 JSON 对象。"));
		}
		return false;
	}
	const TSharedPtr<FJsonObject>* WorkObject = nullptr;
	if (!Root->TryGetObjectField(TEXT("work"), WorkObject)
		|| !WorkObject
		|| !ValidateFields(
			*WorkObject,
			{
				TEXT("projectId"),
				TEXT("displayName"),
				TEXT("visibility"),
				TEXT("revision"),
				TEXT("updatedAt"),
				TEXT("latestRelease")
			},
			{},
			TEXT("Registry work"),
			OutFailureCode,
			OutError))
	{
		if (OutFailureCode.IsEmpty())
		{
			return Fail(
				OutFailureCode,
				OutError,
				TEXT("protocol"),
				TEXT("Registry 作品页 work 字段无效。"));
		}
		return false;
	}
	FString ProjectId;
	double Revision = -1;
	const TSharedPtr<FJsonObject>* ReleaseObject = nullptr;
	if (!(*WorkObject)->TryGetStringField(TEXT("projectId"), ProjectId)
		|| !(*WorkObject)->TryGetStringField(
			TEXT("displayName"),
			OutWork.DisplayName)
		|| !(*WorkObject)->TryGetStringField(
			TEXT("visibility"),
			OutWork.Visibility)
		|| !(*WorkObject)->TryGetNumberField(TEXT("revision"), Revision)
		|| !(*WorkObject)->TryGetStringField(
			TEXT("updatedAt"),
			OutWork.UpdatedAt)
		|| !(*WorkObject)->TryGetObjectField(
			TEXT("latestRelease"),
			ReleaseObject)
		|| !ReleaseObject
		|| !ParseCanonicalGuid(ProjectId, OutWork.ProjectId)
		|| OutWork.ProjectId != ExpectedProjectId
		|| !IsDisplayName(OutWork.DisplayName)
		|| (OutWork.Visibility != TEXT("private")
			&& OutWork.Visibility != TEXT("public"))
		|| !FMath::IsFinite(Revision)
		|| Revision < 1
		|| Revision > MaxJsonSafeInteger
		|| FMath::FloorToDouble(Revision) != Revision
		|| !IsTimestamp(OutWork.UpdatedAt)
		|| !ParsePublishedRelease(
			*ReleaseObject,
			ExpectedProjectId,
			OutWork.LatestReleaseId,
			OutFailureCode,
			OutError))
	{
		if (OutFailureCode.IsEmpty())
		{
			return Fail(
				OutFailureCode,
				OutError,
				TEXT("protocol"),
				TEXT("Registry 作品页字段无效或与请求 Project 不一致。"));
		}
		return false;
	}
	OutWork.Revision = static_cast<int64>(Revision);
	return true;
}

FAkUGCHttpRequestPtr
FAkUGCPlayerReleaseRegistryClient::StartDownload(
	const FString& BaseUrl,
	const FString& BearerToken,
	const FGuid& ReleaseId,
	float TimeoutSeconds,
	FString& OutExpectedUrl,
	FString& OutError)
{
	OutExpectedUrl.Reset();
	OutError.Reset();
	FString Normalized;
	if (!NormalizeBaseUrl(BaseUrl, Normalized, OutError)
		|| !ValidateBearerToken(BearerToken, OutError))
	{
		return nullptr;
	}
	if (!ReleaseId.IsValid())
	{
		OutError = TEXT("Release ID 无效。");
		return nullptr;
	}
	OutExpectedUrl =
		Normalized
		+ TEXT("/v1/releases/")
		+ ReleaseId.ToString(EGuidFormats::DigitsWithHyphensLower)
		+ TEXT("/download");

	FAkUGCHttpRequestSpec Spec;
	Spec.Method = EAkUGCHttpMethod::Get;
	Spec.Url = OutExpectedUrl;
	Spec.BearerToken = BearerToken;
	Spec.TimeoutSeconds = TimeoutSeconds;
	Spec.MaxResponseBytes = MaxArtifactBytes;
	Spec.MaxResponseHeaderBytes = MaxResponseHeaderBytes;
	Spec.bAllowLoopbackHttpWithBearer = true;
	Spec.bAcceptCompressedResponse = false;
	FAkUGCHttpRequestPtr Request =
		FAkUGCHttpTransport::Start(MoveTemp(Spec));
	if (!Request.IsValid())
	{
		OutError =
			TEXT("Registry 传输不可用、忙碌或请求配置无效。");
		OutExpectedUrl.Reset();
	}
	return Request;
}

bool FAkUGCPlayerReleaseRegistryClient::ImportDownloaded(
	const FAkUGCHttpResult& Transport,
	const FString& ExpectedUrl,
	const FGuid& ExpectedReleaseId,
	const FString& IndependentlyTrustedPublicKey,
	const FString& LibraryDirectory,
	FAkUGCWorkEntry& OutEntry,
	FString& OutArtifactSha256,
	FString& OutFailureCode,
	FString& OutError)
{
	using namespace AkUGCPlayerRegistryPrivate;
	OutEntry = {};
	OutArtifactSha256.Reset();
	OutFailureCode.Reset();
	OutError.Reset();

	FString TrustedKey;
	FString ConfigurationError;
	if (ExpectedUrl.IsEmpty()
		|| !ExpectedReleaseId.IsValid()
		|| LibraryDirectory.IsEmpty()
		|| !NormalizeTrustedPublicKey(
			IndependentlyTrustedPublicKey,
			TrustedKey,
			ConfigurationError))
	{
		return Fail(
			OutFailureCode,
			OutError,
			TEXT("invalid_configuration"),
			ConfigurationError.IsEmpty()
				? TEXT("Player Registry 下载上下文无效。")
				: ConfigurationError);
	}
	if (Transport.bTooLarge)
	{
		return Fail(
			OutFailureCode,
			OutError,
			TEXT("response_too_large"),
			TEXT("Registry 制品超过 16 MiB，已在接收时中止。"));
	}
	if (Transport.bHeadersTooLarge)
	{
		return Fail(
			OutFailureCode,
			OutError,
			TEXT("header_too_large"),
			TEXT("Registry 响应 Header 超过 64 KiB，已中止。"));
	}
	if (Transport.bTimedOut)
	{
		return Fail(
			OutFailureCode,
			OutError,
			TEXT("timeout"),
			TEXT("Registry 下载超时。"));
	}
	if (Transport.bCancelled)
	{
		return Fail(
			OutFailureCode,
			OutError,
			TEXT("cancelled"),
			TEXT("Registry 下载已取消。"));
	}
	if (Transport.NativeCode != 0)
	{
		return Fail(
			OutFailureCode,
			OutError,
			TEXT("transport"),
			FString::Printf(
				TEXT("Registry 下载传输失败（传输码 %d）。"),
				Transport.NativeCode));
	}
	if (Transport.EffectiveUrl != ExpectedUrl)
	{
		return Fail(
			OutFailureCode,
			OutError,
			TEXT("redirect"),
			TEXT("Registry 下载最终地址与请求地址不一致，已拒绝重定向。"));
	}
	if (Transport.StatusCode >= 300 && Transport.StatusCode < 400)
	{
		return Fail(
			OutFailureCode,
			OutError,
			TEXT("redirect"),
			FString::Printf(
				TEXT("Registry 下载拒绝 HTTP 重定向（%d）。"),
				Transport.StatusCode));
	}
	if (Transport.StatusCode != 200)
	{
		const TCHAR* Code = TEXT("http_status");
		if (Transport.StatusCode == 401)
		{
			Code = TEXT("unauthorized");
		}
		else if (Transport.StatusCode == 403)
		{
			Code = TEXT("forbidden");
		}
		else if (Transport.StatusCode == 404)
		{
			Code = TEXT("not_available");
		}
		return Fail(
			OutFailureCode,
			OutError,
			Code,
			FString::Printf(
				TEXT("Registry 下载被拒绝（HTTP %d）。"),
				Transport.StatusCode));
	}
	if (Transport.Body.IsEmpty()
		|| Transport.Body.Num() > MaxArtifactBytes)
	{
		return Fail(
			OutFailureCode,
			OutError,
			TEXT("response_too_large"),
			TEXT("Registry 制品为空或超过 16 MiB。"));
	}
	if (HasHeader(Transport, TEXT("Location"))
		|| HasHeader(Transport, TEXT("Content-Encoding"))
		|| HasHeader(Transport, TEXT("Transfer-Encoding")))
	{
		return Fail(
			OutFailureCode,
			OutError,
			TEXT("protocol"),
			TEXT("Registry 制品响应包含禁止的重定向或传输编码 Header。"));
	}

	FString ContentType;
	FString ContentLengthText;
	FString ETag;
	FString ContentHash;
	if (!FindSingleHeader(
			Transport,
			TEXT("Content-Type"),
			ContentType,
			OutFailureCode,
			OutError)
		|| !FindSingleHeader(
			Transport,
			TEXT("Content-Length"),
			ContentLengthText,
			OutFailureCode,
			OutError)
		|| !FindSingleHeader(
			Transport,
			TEXT("ETag"),
			ETag,
			OutFailureCode,
			OutError)
		|| !FindSingleHeader(
			Transport,
			TEXT("X-AkUGC-Content-Hash"),
			ContentHash,
			OutFailureCode,
			OutError))
	{
		return false;
	}
	int64 ContentLength = -1;
	if (ContentType != TEXT("application/vnd.akugc+json")
		|| !ParseDecimal(ContentLengthText, ContentLength)
		|| ContentLength != Transport.Body.Num()
		|| !IsLowerHex(ContentHash, 64))
	{
		return Fail(
			OutFailureCode,
			OutError,
			TEXT("protocol"),
			TEXT("Registry 制品响应的类型、长度或 ContentHash Header 无效。"));
	}

	OutArtifactSha256 =
		FAkUGCLogicPackHasher::Sha256Hex(Transport.Body);
	const FString ExpectedETag = FString::Printf(
		TEXT("\"sha256:%s\""),
		*OutArtifactSha256);
	if (ETag != ExpectedETag)
	{
		OutArtifactSha256.Reset();
		return Fail(
			OutFailureCode,
			OutError,
			TEXT("artifact_digest"),
			TEXT("Registry 制品字节与 Artifact SHA-256 ETag 不一致。"));
	}

	FString ImportError;
	if (!FAkUGCWorkLibrary(LibraryDirectory).ImportArtifact(
			Transport.Body,
			TrustedKey,
			ExpectedReleaseId,
			ContentHash,
			OutEntry,
			&ImportError))
	{
		OutArtifactSha256.Reset();
		return Fail(
			OutFailureCode,
			OutError,
			TEXT("local_verification"),
			TEXT("下载完成，但本地签名、内容或入库验证失败：")
				+ ImportError);
	}
	return true;
}

bool FAkUGCPlayerReleaseRegistryClient::NormalizeBaseUrl(
	const FString& BaseUrl,
	FString& OutNormalized,
	FString& OutError)
{
	using namespace AkUGCPlayerRegistryPrivate;
	OutNormalized.Reset();
	OutError.Reset();
	FString Candidate = BaseUrl.TrimStartAndEnd();
	if (Candidate.EndsWith(TEXT("/")))
	{
		Candidate.LeftChopInline(1);
	}
	if (!Candidate.StartsWith(
			TEXT("http://"),
			ESearchCase::CaseSensitive)
		|| Candidate.Len()
			> AkUGCInputBudget::MaxCopilotUrlCharacters)
	{
		return FailConfiguration(
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
			return FailConfiguration(
				OutError,
				TEXT("Registry 地址包含禁止字符。"));
		}
	}
	const FString Authority = Candidate.Mid(7);
	if (Authority.IsEmpty() || Authority.Contains(TEXT("/")))
	{
		return FailConfiguration(
			OutError,
			TEXT("Registry 地址不能包含路径。"));
	}
	FString Host = Authority;
	int32 Colon = INDEX_NONE;
	if (Authority.FindChar(TEXT(':'), Colon))
	{
		Host = Authority.Left(Colon);
		const FString Port = Authority.Mid(Colon + 1);
		if (Port.IsEmpty() || Port.Len() > 5)
		{
			return FailConfiguration(
				OutError,
				TEXT("Registry 端口无效。"));
		}
		for (const TCHAR Character : Port)
		{
			if (Character < TEXT('0') || Character > TEXT('9'))
			{
				return FailConfiguration(
					OutError,
					TEXT("Registry 端口无效。"));
			}
		}
		const int32 Number = FCString::Atoi(*Port);
		if (Number < 1 || Number > 65535)
		{
			return FailConfiguration(
				OutError,
				TEXT("Registry 端口无效。"));
		}
	}
	if (!Host.Equals(TEXT("localhost"), ESearchCase::IgnoreCase)
		&& Host != TEXT("127.0.0.1"))
	{
		return FailConfiguration(
			OutError,
			TEXT("P19 静态 token 只允许发送到 localhost 或 127.0.0.1。"));
	}
	OutNormalized = Candidate;
	return true;
}

bool FAkUGCPlayerReleaseRegistryClient::ValidateBearerToken(
	const FString& BearerToken,
	FString& OutError)
{
	using namespace AkUGCPlayerRegistryPrivate;
	OutError.Reset();
	if (BearerToken.IsEmpty()
		|| BearerToken.Len()
			> AkUGCInputBudget::MaxCopilotApiKeyBytes)
	{
		return FailConfiguration(
			OutError,
			TEXT("Player token 为空或超过 4096 字节。"));
	}
	for (const TCHAR Character : BearerToken)
	{
		if (Character <= 32 || Character >= 127)
		{
			return FailConfiguration(
				OutError,
				TEXT("Player token 只能包含可见 ASCII 字符。"));
		}
	}
	return true;
}

bool FAkUGCPlayerReleaseRegistryClient::NormalizeTrustedPublicKey(
	const FString& PublicKey,
	FString& OutNormalized,
	FString& OutError)
{
	using namespace AkUGCPlayerRegistryPrivate;
	OutError.Reset();
	OutNormalized = PublicKey.TrimStartAndEnd().ToLower();
	if (!IsLowerHex(OutNormalized, 64))
	{
		OutNormalized.Reset();
		return FailConfiguration(
			OutError,
			TEXT("独立信任公钥必须是 64 位 Ed25519 hex。"));
	}
	return true;
}

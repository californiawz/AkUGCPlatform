#include "Release/AkUGCPlayerReleaseRegistryClient.h"

#include "Pack/AkUGCLogicPackHasher.h"
#include "Validation/AkUGCInputBudget.h"

namespace AkUGCPlayerRegistryPrivate
{
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
					TEXT("Registry 下载响应必须且只能包含一个 %s Header。"),
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

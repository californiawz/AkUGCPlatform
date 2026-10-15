#include "Network/AkUGCHttpTransport.h"

#include <string>
#include <thread>

#include "Containers/StringConv.h"
#include "Interfaces/ISslCertificateManager.h"
#include "Interfaces/ISslManager.h"
#include "Misc/ScopeLock.h"
#include "SslModule.h"
#include "Validation/AkUGCInputBudget.h"

#if AKUGC_HTTP_CURL
THIRD_PARTY_INCLUDES_START
#include "curl/curl.h"
THIRD_PARTY_INCLUDES_END

namespace AkUGCHttpCurl
{
	int32 InitializationCount = 0;
	ISslManager* Ssl = nullptr;
	ISslCertificateManager* Certificates = nullptr;
	TArray<TUniquePtr<std::thread>> Workers;
	TArray<TWeakPtr<FAkUGCHttpRequest, ESPMode::ThreadSafe>> Requests;

	struct FReceive
	{
		FAkUGCHttpRequestPtr Request;
		FAkUGCHttpResult Result;
		int32 MaxBodyBytes = 0;
		int32 MaxHeaderBytes = 0;
		int64 HeaderBytes = 0;
	};

	bool IsSafeHeaderValue(
		const FString& Value,
		int32 MaxCharacters,
		bool bAllowEmpty)
	{
		if ((!bAllowEmpty && Value.IsEmpty()) || Value.Len() > MaxCharacters)
		{
			return false;
		}
		for (const TCHAR Character : Value)
		{
			if (Character <= 32 || Character >= 127)
			{
				return false;
			}
		}
		return true;
	}

	bool IsLoopbackHttpUrl(const FString& Url)
	{
		if (!Url.StartsWith(TEXT("http://"), ESearchCase::CaseSensitive))
		{
			return false;
		}
		const FString Rest = Url.Mid(7);
		int32 Slash = INDEX_NONE;
		const FString Authority = Rest.FindChar(TEXT('/'), Slash)
			? Rest.Left(Slash)
			: Rest;
		FString Host = Authority;
		int32 Colon = INDEX_NONE;
		if (Authority.FindChar(TEXT(':'), Colon))
		{
			Host = Authority.Left(Colon);
		}
		return Host.Equals(TEXT("localhost"), ESearchCase::IgnoreCase)
			|| Host == TEXT("127.0.0.1");
	}

	bool IsValidSpec(const FAkUGCHttpRequestSpec& Spec)
	{
		if (InitializationCount <= 0
			|| Spec.Url.IsEmpty()
			|| Spec.Url.Len() > AkUGCInputBudget::MaxCopilotUrlCharacters
			|| !FMath::IsFinite(Spec.TimeoutSeconds)
			|| Spec.TimeoutSeconds <= 0.0f
			|| Spec.TimeoutSeconds > 120.0f
			|| Spec.MaxResponseBytes <= 0
			|| Spec.MaxResponseBytes > AkUGCInputBudget::MaxLogicPackBytes
			|| Spec.MaxResponseHeaderBytes <= 0
			|| Spec.MaxResponseHeaderBytes
				> AkUGCInputBudget::MaxCopilotResponseBytes
			|| Spec.Body.Num() > AkUGCInputBudget::MaxLogicPackBytes
			|| !IsSafeHeaderValue(
				Spec.BearerToken,
				AkUGCInputBudget::MaxCopilotApiKeyBytes,
				true)
			|| !IsSafeHeaderValue(Spec.ContentType, 128, true)
			|| !IsSafeHeaderValue(Spec.IdempotencyKey, 128, true)
			|| Spec.Url.Contains(TEXT("@"))
			|| Spec.Url.Contains(TEXT("\\"))
			|| Spec.Url.Contains(TEXT("#"))
			|| Spec.Url.Contains(TEXT("?"))
			|| (!Spec.Url.StartsWith(
					TEXT("https://"),
					ESearchCase::CaseSensitive)
				&& !Spec.Url.StartsWith(
					TEXT("http://"),
					ESearchCase::CaseSensitive)))
		{
			return false;
		}
		for (const TCHAR Character : Spec.Url)
		{
			if (Character <= 32 || Character >= 127)
			{
				return false;
			}
		}
		if (!Spec.BearerToken.IsEmpty()
			&& Spec.Url.StartsWith(
				TEXT("http://"),
				ESearchCase::CaseSensitive)
			&& (!Spec.bAllowLoopbackHttpWithBearer
				|| !IsLoopbackHttpUrl(Spec.Url)))
		{
			return false;
		}
#if WITH_DEV_AUTOMATION_TESTS
		if (Spec.TestCaBundle.Len() > 4096)
		{
			return false;
		}
#endif
		return true;
	}

	bool ParseContentLength(const FString& Value, int64& OutLength)
	{
		if (Value.IsEmpty())
		{
			return false;
		}
		uint64 Result = 0;
		for (const TCHAR Character : Value)
		{
			if (Character < TEXT('0') || Character > TEXT('9'))
			{
				return false;
			}
			const uint64 Digit = static_cast<uint64>(Character - TEXT('0'));
			if (Result
				> (static_cast<uint64>(MAX_int64) - Digit) / 10ULL)
			{
				return false;
			}
			Result = Result * 10ULL + Digit;
		}
		OutLength = static_cast<int64>(Result);
		return true;
	}

	size_t Header(char* Data, size_t Size, size_t Count, void* User)
	{
		FReceive& Receive = *static_cast<FReceive*>(User);
		if (Receive.Request->IsCancelled()
			|| (Size != 0 && Count > MAX_uint64 / Size))
		{
			return 0;
		}
		const size_t Length = Size * Count;
		if (Length > static_cast<size_t>(MAX_int64)
			|| Receive.HeaderBytes
				> Receive.MaxHeaderBytes - static_cast<int64>(Length))
		{
			Receive.Result.bHeadersTooLarge = true;
			return 0;
		}
		Receive.HeaderBytes += static_cast<int64>(Length);

		int32 TextLength = static_cast<int32>(Length);
		while (TextLength > 0
			&& (Data[TextLength - 1] == '\r'
				|| Data[TextLength - 1] == '\n'))
		{
			--TextLength;
		}
		int32 Colon = INDEX_NONE;
		bool bAscii = true;
		for (int32 Index = 0; Index < TextLength; ++Index)
		{
			const uint8 Character = static_cast<uint8>(Data[Index]);
			if (Character == 0 || Character >= 127
				|| (Character < 32 && Character != '\t'))
			{
				bAscii = false;
				break;
			}
			if (Colon == INDEX_NONE && Character == ':')
			{
				Colon = Index;
			}
		}
		if (bAscii && Colon > 0)
		{
			TArray<ANSICHAR> Line;
			Line.Append(Data, TextLength);
			Line.Add('\0');
			FString Text(ANSI_TO_TCHAR(Line.GetData()));
			FString Name = Text.Left(Colon);
			FString Value = Text.Mid(Colon + 1);
			Name.TrimStartAndEndInline();
			Value.TrimStartAndEndInline();
			bool bValidName = !Name.IsEmpty();
			for (const TCHAR Character : Name)
			{
				bValidName &= (Character >= TEXT('0')
						&& Character <= TEXT('9'))
					|| (Character >= TEXT('A')
						&& Character <= TEXT('Z'))
					|| (Character >= TEXT('a')
						&& Character <= TEXT('z'))
					|| Character == TEXT('-');
			}
			if (bValidName)
			{
				FAkUGCHttpHeader& Stored =
					Receive.Result.Headers.AddDefaulted_GetRef();
				Stored.Name = MoveTemp(Name);
				Stored.Value = MoveTemp(Value);
				if (Stored.Name.Equals(
						TEXT("Content-Length"),
						ESearchCase::IgnoreCase))
				{
					int64 ContentLength = 0;
					if (ParseContentLength(Stored.Value, ContentLength)
						&& ContentLength > Receive.MaxBodyBytes)
					{
						Receive.Result.bTooLarge = true;
						return 0;
					}
				}
			}
		}
		return Length;
	}

	size_t Write(char* Data, size_t Size, size_t Count, void* User)
	{
		FReceive& Receive = *static_cast<FReceive*>(User);
		if (Receive.Request->IsCancelled())
		{
			return 0;
		}
		if (Size != 0 && Count > MAX_uint64 / Size)
		{
			Receive.Result.bTooLarge = true;
			return 0;
		}
		const size_t Length = Size * Count;
		const int32 Remaining =
			Receive.MaxBodyBytes - Receive.Result.Body.Num();
		if (Remaining < 0
			|| Length > static_cast<size_t>(Remaining))
		{
			Receive.Result.bTooLarge = true;
			return 0;
		}
		if (Length > 0)
		{
			Receive.Result.Body.Append(
				reinterpret_cast<const uint8*>(Data),
				static_cast<int32>(Length));
		}
		return Length;
	}

	CURLcode ConfigureSsl(CURL*, void* Context, void*)
	{
		if (!Certificates || !Certificates->HasCertificatesAvailable())
		{
			return CURLE_SSL_CERTPROBLEM;
		}
		Certificates->AddCertificatesToSslContext(
			static_cast<SSL_CTX*>(Context));
		return CURLE_OK;
	}
}
#endif

FAkUGCHttpResult FAkUGCHttpRequest::TakeResult()
{
	FScopeLock Lock(&Mutex);
	return MoveTemp(Result);
}

bool FAkUGCHttpTransport::Initialize()
{
	check(IsInGameThread());
#if AKUGC_HTTP_CURL
	using namespace AkUGCHttpCurl;
	if (InitializationCount > 0)
	{
		++InitializationCount;
		return true;
	}
	FSslModule& Module = FSslModule::Get();
	Certificates = &Module.GetCertificateManager();
	long Flags = CURL_GLOBAL_ALL;
#if IS_MONOLITHIC || UE_MERGED_MODULES
	Ssl = &Module.GetSslManager();
	if (!Ssl->InitializeSsl())
	{
		Ssl = nullptr;
		Certificates = nullptr;
		return false;
	}
	Flags &= ~CURL_GLOBAL_SSL;
#endif
	if (curl_global_init(Flags) != CURLE_OK)
	{
		if (Ssl)
		{
			Ssl->ShutdownSsl();
		}
		Ssl = nullptr;
		Certificates = nullptr;
		return false;
	}
	InitializationCount = 1;
	return true;
#else
	return false;
#endif
}

void FAkUGCHttpTransport::Shutdown()
{
	check(IsInGameThread());
#if AKUGC_HTTP_CURL
	using namespace AkUGCHttpCurl;
	if (InitializationCount <= 0)
	{
		return;
	}
	if (--InitializationCount > 0)
	{
		return;
	}
	for (const TWeakPtr<FAkUGCHttpRequest, ESPMode::ThreadSafe>& Weak
		: Requests)
	{
		if (const FAkUGCHttpRequestPtr Request = Weak.Pin())
		{
			Request->Cancel();
		}
	}
	for (TUniquePtr<std::thread>& Worker : Workers)
	{
		if (Worker->joinable())
		{
			Worker->join();
		}
	}
	Workers.Reset();
	Requests.Reset();
	curl_global_cleanup();
	if (Ssl)
	{
		Ssl->ShutdownSsl();
	}
	Ssl = nullptr;
	Certificates = nullptr;
#endif
}

FAkUGCHttpRequestPtr FAkUGCHttpTransport::Start(
	FAkUGCHttpRequestSpec Spec)
{
	check(IsInGameThread());
#if AKUGC_HTTP_CURL
	using namespace AkUGCHttpCurl;
	if (!IsValidSpec(Spec))
	{
		return nullptr;
	}
	for (int32 Index = Workers.Num() - 1; Index >= 0; --Index)
	{
		const FAkUGCHttpRequestPtr Existing = Requests[Index].Pin();
		if (!Existing.IsValid() || Existing->IsDone())
		{
			if (Workers[Index]->joinable())
			{
				Workers[Index]->join();
			}
			Workers.RemoveAt(Index);
			Requests.RemoveAt(Index);
		}
	}
	if (Workers.Num() >= 4)
	{
		return nullptr;
	}

	const FAkUGCHttpRequestPtr Request =
		MakeShared<FAkUGCHttpRequest, ESPMode::ThreadSafe>();
	Requests.Add(Request);
	Workers.Add(MakeUnique<std::thread>(
		[Request, Spec = MoveTemp(Spec)]() mutable
		{
			FReceive Receive;
			Receive.Request = Request;
			Receive.MaxBodyBytes = Spec.MaxResponseBytes;
			Receive.MaxHeaderBytes = Spec.MaxResponseHeaderBytes;
			Receive.Result.Body.Reserve(
				FMath::Min(Spec.MaxResponseBytes, 64 * 1024));

			FTCHARToUTF8 UrlBytes(*Spec.Url);
			FTCHARToUTF8 TokenBytes(*Spec.BearerToken);
			FTCHARToUTF8 ContentTypeBytes(*Spec.ContentType);
			FTCHARToUTF8 IdempotencyBytes(*Spec.IdempotencyKey);
#if WITH_DEV_AUTOMATION_TESTS
			FTCHARToUTF8 CaBytes(*Spec.TestCaBundle);
#endif
			CURL* Handle = curl_easy_init();
			curl_slist* Headers = nullptr;
			CURLcode Code = Handle ? CURLE_OK : CURLE_FAILED_INIT;
			if (Handle)
			{
				const auto Set = [&](CURLoption Option, auto Value)
				{
					if (Code == CURLE_OK)
					{
						Code = curl_easy_setopt(Handle, Option, Value);
					}
				};
				const auto AddHeader = [&](const std::string& Header)
				{
					if (Code != CURLE_OK)
					{
						return;
					}
					curl_slist* Added =
						curl_slist_append(Headers, Header.c_str());
					if (Added)
					{
						Headers = Added;
					}
					else
					{
						Code = CURLE_OUT_OF_MEMORY;
					}
				};

				if (!Spec.ContentType.IsEmpty())
				{
					AddHeader(
						std::string("Content-Type: ")
						+ ContentTypeBytes.Get());
				}
				if (!Spec.BearerToken.IsEmpty())
				{
					AddHeader(
						std::string("Authorization: Bearer ")
						+ TokenBytes.Get());
				}
				if (!Spec.IdempotencyKey.IsEmpty())
				{
					AddHeader(
						std::string("Idempotency-Key: ")
						+ IdempotencyBytes.Get());
				}

				Set(CURLOPT_URL, UrlBytes.Get());
				Set(CURLOPT_PROTOCOLS_STR, "http,https");
				Set(CURLOPT_FOLLOWLOCATION, 0L);
				Set(CURLOPT_MAXREDIRS, 0L);
				Set(CURLOPT_UNRESTRICTED_AUTH, 0L);
				Set(CURLOPT_PROXY, "");
				Set(CURLOPT_NETRC, static_cast<long>(CURL_NETRC_IGNORED));
				Set(CURLOPT_NOSIGNAL, 1L);
				Set(CURLOPT_SSL_VERIFYPEER, 1L);
				Set(CURLOPT_SSL_VERIFYHOST, 2L);
				if (Spec.Method == EAkUGCHttpMethod::Post)
				{
					static const uint8 EmptyBody = 0;
					Set(CURLOPT_POST, 1L);
					Set(
						CURLOPT_POSTFIELDS,
						Spec.Body.IsEmpty()
							? static_cast<const void*>(&EmptyBody)
							: static_cast<const void*>(
								Spec.Body.GetData()));
					Set(
						CURLOPT_POSTFIELDSIZE_LARGE,
						static_cast<curl_off_t>(Spec.Body.Num()));
				}
				else
				{
					Set(CURLOPT_HTTPGET, 1L);
				}
				if (Headers)
				{
					Set(CURLOPT_HTTPHEADER, Headers);
				}
				Set(
					CURLOPT_TIMEOUT_MS,
					static_cast<long>(
						FMath::Max(
							1.0f,
							Spec.TimeoutSeconds * 1000.0f)));
				Set(
					CURLOPT_CONNECTTIMEOUT_MS,
					static_cast<long>(
						FMath::Min(
							10000.0f,
							FMath::Max(
								1.0f,
								Spec.TimeoutSeconds * 1000.0f))));
				Set(CURLOPT_HEADERFUNCTION, &Header);
				Set(CURLOPT_HEADERDATA, &Receive);
				Set(CURLOPT_WRITEFUNCTION, &Write);
				Set(CURLOPT_WRITEDATA, &Receive);
				Set(CURLOPT_NOPROGRESS, 0L);
				Set(
					CURLOPT_XFERINFOFUNCTION,
					+[](void* User, curl_off_t, curl_off_t, curl_off_t,
						   curl_off_t) -> int
					{
						return static_cast<FAkUGCHttpRequest*>(User)
							->IsCancelled()
							? 1
							: 0;
					});
				Set(CURLOPT_XFERINFODATA, Request.Get());
				if (Spec.bAcceptCompressedResponse)
				{
					Set(CURLOPT_ACCEPT_ENCODING, "");
				}
				if (Spec.Url.StartsWith(
						TEXT("https://"),
						ESearchCase::CaseSensitive))
				{
#if WITH_DEV_AUTOMATION_TESTS
					if (!Spec.TestCaBundle.IsEmpty())
					{
						Set(CURLOPT_CAINFO, CaBytes.Get());
					}
					else
#endif
					{
						Set(
							CURLOPT_CAINFO,
							static_cast<const char*>(nullptr));
						Set(CURLOPT_SSL_CTX_FUNCTION, &ConfigureSsl);
					}
				}
				if (Code == CURLE_OK)
				{
					Code = curl_easy_perform(Handle);
				}
				long Status = 0;
				curl_easy_getinfo(
					Handle,
					CURLINFO_RESPONSE_CODE,
					&Status);
				Receive.Result.StatusCode = static_cast<int32>(Status);
				char* EffectiveUrl = nullptr;
				if (curl_easy_getinfo(
						Handle,
						CURLINFO_EFFECTIVE_URL,
						&EffectiveUrl)
						== CURLE_OK
					&& EffectiveUrl)
				{
					Receive.Result.EffectiveUrl =
						UTF8_TO_TCHAR(EffectiveUrl);
				}
				curl_slist_free_all(Headers);
				curl_easy_cleanup(Handle);
			}
			Receive.Result.NativeCode = static_cast<int32>(Code);
			Receive.Result.bTimedOut =
				Code == CURLE_OPERATION_TIMEDOUT;
			Receive.Result.bCancelled = Request->IsCancelled();
			{
				FScopeLock Lock(&Request->Mutex);
				Request->Result = MoveTemp(Receive.Result);
			}
			Request->bDone.Store(true);
		}));
	return Request;
#else
	return nullptr;
#endif
}

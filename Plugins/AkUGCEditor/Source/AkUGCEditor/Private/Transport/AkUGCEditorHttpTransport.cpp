#include "Transport/AkUGCEditorHttpTransport.h"

#include <string>
#include <thread>

#include "Containers/StringConv.h"
#include "Interfaces/ISslCertificateManager.h"
#include "Interfaces/ISslManager.h"
#include "Misc/ScopeLock.h"
#include "SslModule.h"
#include "Validation/AkUGCInputBudget.h"

#if AKUGC_COPILOT_CURL
THIRD_PARTY_INCLUDES_START
#include "curl/curl.h"
THIRD_PARTY_INCLUDES_END

namespace AkUGCEditorHttpCurl
{
	bool bInitialized = false;
	ISslManager* Ssl = nullptr;
	ISslCertificateManager* Certificates = nullptr;
	TArray<TUniquePtr<std::thread>> Workers;
	TArray<TWeakPtr<FAkUGCEditorHttpRequest, ESPMode::ThreadSafe>> Requests;

	struct FReceive
	{
		FAkUGCEditorHttpRequestPtr Request;
		FAkUGCEditorHttpResult Result;
		int32 MaxBodyBytes = 0;
	};

	bool IsSafeHeaderValue(const FString& Value, int32 MaxCharacters, bool bAllowEmpty)
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

	bool IsValidSpec(const FAkUGCEditorHttpRequestSpec& Spec)
	{
		if (!bInitialized
			|| Spec.Url.IsEmpty()
			|| Spec.Url.Len() > AkUGCInputBudget::MaxCopilotUrlCharacters
			|| !FMath::IsFinite(Spec.TimeoutSeconds)
			|| Spec.TimeoutSeconds <= 0.0f
			|| Spec.TimeoutSeconds > 120.0f
			|| Spec.MaxResponseBytes <= 0
			|| Spec.MaxResponseBytes > AkUGCInputBudget::MaxLogicPackBytes
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
			|| (!Spec.Url.StartsWith(TEXT("https://"), ESearchCase::CaseSensitive)
				&& !Spec.Url.StartsWith(TEXT("http://"), ESearchCase::CaseSensitive)))
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
			&& Spec.Url.StartsWith(TEXT("http://"), ESearchCase::CaseSensitive)
			&& (!Spec.bAllowLoopbackHttpWithBearer || !IsLoopbackHttpUrl(Spec.Url)))
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
		const int32 Remaining = Receive.MaxBodyBytes - Receive.Result.Body.Num();
		if (Remaining < 0 || Length > static_cast<size_t>(Remaining))
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
		Certificates->AddCertificatesToSslContext(static_cast<SSL_CTX*>(Context));
		return CURLE_OK;
	}
}
#endif

FAkUGCEditorHttpResult FAkUGCEditorHttpRequest::TakeResult()
{
	FScopeLock Lock(&Mutex);
	return MoveTemp(Result);
}

bool FAkUGCEditorHttpTransport::Initialize()
{
	check(IsInGameThread());
#if AKUGC_COPILOT_CURL
	using namespace AkUGCEditorHttpCurl;
	if (bInitialized)
	{
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
	bInitialized = true;
	return true;
#else
	return false;
#endif
}

void FAkUGCEditorHttpTransport::Shutdown()
{
	check(IsInGameThread());
#if AKUGC_COPILOT_CURL
	using namespace AkUGCEditorHttpCurl;
	if (!bInitialized)
	{
		return;
	}
	for (const TWeakPtr<FAkUGCEditorHttpRequest, ESPMode::ThreadSafe>& Weak : Requests)
	{
		if (const FAkUGCEditorHttpRequestPtr Request = Weak.Pin())
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
	bInitialized = false;
#endif
}

FAkUGCEditorHttpRequestPtr FAkUGCEditorHttpTransport::Start(
	FAkUGCEditorHttpRequestSpec Spec)
{
	check(IsInGameThread());
#if AKUGC_COPILOT_CURL
	using namespace AkUGCEditorHttpCurl;
	if (!IsValidSpec(Spec))
	{
		return nullptr;
	}
	for (int32 Index = Workers.Num() - 1; Index >= 0; --Index)
	{
		const FAkUGCEditorHttpRequestPtr Existing = Requests[Index].Pin();
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

	const FAkUGCEditorHttpRequestPtr Request =
		MakeShared<FAkUGCEditorHttpRequest, ESPMode::ThreadSafe>();
	Requests.Add(Request);
	Workers.Add(MakeUnique<std::thread>(
		[Request, Spec = MoveTemp(Spec)]() mutable
		{
			FReceive Receive;
			Receive.Request = Request;
			Receive.MaxBodyBytes = Spec.MaxResponseBytes;
			Receive.Result.Body.Reserve(FMath::Min(Spec.MaxResponseBytes, 64 * 1024));

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
					curl_slist* Added = curl_slist_append(Headers, Header.c_str());
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
					AddHeader(std::string("Content-Type: ") + ContentTypeBytes.Get());
				}
				if (!Spec.BearerToken.IsEmpty())
				{
					AddHeader(std::string("Authorization: Bearer ") + TokenBytes.Get());
				}
				if (!Spec.IdempotencyKey.IsEmpty())
				{
					AddHeader(std::string("Idempotency-Key: ") + IdempotencyBytes.Get());
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
				if (Spec.Method == EAkUGCEditorHttpMethod::Post)
				{
					static const uint8 EmptyBody = 0;
					Set(CURLOPT_POST, 1L);
					Set(
						CURLOPT_POSTFIELDS,
						Spec.Body.IsEmpty()
							? static_cast<const void*>(&EmptyBody)
							: static_cast<const void*>(Spec.Body.GetData()));
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
						FMath::Max(1.0f, Spec.TimeoutSeconds * 1000.0f)));
				Set(
					CURLOPT_CONNECTTIMEOUT_MS,
					static_cast<long>(
						FMath::Min(
							10000.0f,
							FMath::Max(1.0f, Spec.TimeoutSeconds * 1000.0f))));
				Set(CURLOPT_WRITEFUNCTION, &Write);
				Set(CURLOPT_WRITEDATA, &Receive);
				Set(CURLOPT_NOPROGRESS, 0L);
				Set(
					CURLOPT_XFERINFOFUNCTION,
					+[](void* User, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int
					{
						return static_cast<FAkUGCEditorHttpRequest*>(User)
							->IsCancelled()
							? 1
							: 0;
					});
				Set(CURLOPT_XFERINFODATA, Request.Get());
				Set(CURLOPT_ACCEPT_ENCODING, "");
				if (Spec.Url.StartsWith(TEXT("https://"), ESearchCase::CaseSensitive))
				{
#if WITH_DEV_AUTOMATION_TESTS
					if (!Spec.TestCaBundle.IsEmpty())
					{
						Set(CURLOPT_CAINFO, CaBytes.Get());
					}
					else
#endif
					{
						Set(CURLOPT_CAINFO, static_cast<const char*>(nullptr));
						Set(CURLOPT_SSL_CTX_FUNCTION, &ConfigureSsl);
					}
				}
				if (Code == CURLE_OK)
				{
					Code = curl_easy_perform(Handle);
				}
				long Status = 0;
				curl_easy_getinfo(Handle, CURLINFO_RESPONSE_CODE, &Status);
				Receive.Result.StatusCode = static_cast<int32>(Status);
				curl_slist_free_all(Headers);
				curl_easy_cleanup(Handle);
			}
			Receive.Result.NativeCode = static_cast<int32>(Code);
			Receive.Result.bTimedOut = Code == CURLE_OPERATION_TIMEDOUT;
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

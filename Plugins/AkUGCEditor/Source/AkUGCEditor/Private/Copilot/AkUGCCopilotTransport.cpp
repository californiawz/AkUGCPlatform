#include "Copilot/AkUGCCopilotTransport.h"

#include <thread>
#include "Containers/StringConv.h"
#include "Misc/ScopeLock.h"
#include "SslModule.h"
#include "Interfaces/ISslManager.h"
#include "Interfaces/ISslCertificateManager.h"
#include <string>

#if AKUGC_COPILOT_CURL
THIRD_PARTY_INCLUDES_START
#include "curl/curl.h"
THIRD_PARTY_INCLUDES_END

namespace AkUGCCopilotCurl
{
    bool bInitialized = false;
    ISslManager* Ssl = nullptr;
    ISslCertificateManager* Certificates = nullptr;
    TArray<TUniquePtr<std::thread>> Workers;
    TArray<TWeakPtr<FAkUGCCopilotRequest, ESPMode::ThreadSafe>> Requests;

    struct FReceive
    {
        FAkUGCCopilotRequestPtr Request;
        FAkUGCCopilotTransportResult Result;
    };

    size_t Write(char* Data, size_t Size, size_t Count, void* User)
    {
        auto& Receive = *static_cast<FReceive*>(User);
        if (Receive.Request->IsCancelled()) { return 0; }
        if (Size != 0 && Count > MAX_uint64 / Size) { Receive.Result.bTooLarge = true; return 0; }
        const size_t Length = Size * Count;
        const int32 Remaining =
            FAkUGCCopilotTransport::MaxBodyBytes - Receive.Result.Body.Num();
        if (Remaining < 0 || Length > static_cast<size_t>(Remaining))
        { Receive.Result.bTooLarge = true; return 0; }
        if (Length > 0) { Receive.Result.Body.Append(reinterpret_cast<const uint8*>(Data), static_cast<int32>(Length)); }
        return Length;
    }

    CURLcode ConfigureSsl(CURL*, void* Context, void*)
    {
        if (!Certificates || !Certificates->HasCertificatesAvailable()) { return CURLE_SSL_CERTPROBLEM; }
        Certificates->AddCertificatesToSslContext(static_cast<SSL_CTX*>(Context));
        return CURLE_OK;
    }
}
#endif

FAkUGCCopilotTransportResult FAkUGCCopilotRequest::TakeResult()
{
    FScopeLock Lock(&Mutex);
    return MoveTemp(Result);
}

bool FAkUGCCopilotTransport::Initialize()
{
    check(IsInGameThread());
#if AKUGC_COPILOT_CURL
    using namespace AkUGCCopilotCurl;
    if (bInitialized) { return true; }
    auto& Module = FSslModule::Get();
    Certificates = &Module.GetCertificateManager();
    long Flags = CURL_GLOBAL_ALL;
#if IS_MONOLITHIC || UE_MERGED_MODULES
    Ssl = &Module.GetSslManager();
    if (!Ssl->InitializeSsl()) { Ssl = nullptr; return false; }
    Flags &= ~CURL_GLOBAL_SSL;
#endif
    if (curl_global_init(Flags) != CURLE_OK)
    { if (Ssl) { Ssl->ShutdownSsl(); } Ssl = nullptr; Certificates = nullptr; return false; }
    bInitialized = true;
    return true;
#else
    return false;
#endif
}

void FAkUGCCopilotTransport::Shutdown()
{
    check(IsInGameThread());
#if AKUGC_COPILOT_CURL
    using namespace AkUGCCopilotCurl;
    if (!bInitialized) { return; }
    for (const auto& Weak : Requests) { if (auto Request = Weak.Pin()) { Request->Cancel(); } }
    for (auto& Worker : Workers) { if (Worker->joinable()) { Worker->join(); } }
    Workers.Reset(); Requests.Reset();
    curl_global_cleanup();
    if (Ssl) { Ssl->ShutdownSsl(); }
    Ssl = nullptr; Certificates = nullptr; bInitialized = false;
#endif
}

FAkUGCCopilotRequestPtr FAkUGCCopilotTransport::Start(const FString& Url, const FString& Json,
    const FString& ApiKey, float TimeoutSeconds, const FString& TestCaBundle)
{
    check(IsInGameThread());
#if AKUGC_COPILOT_CURL
    using namespace AkUGCCopilotCurl;
    const bool bJsonValid = AkUGCInputBudget::ValidateJson(
        Json,
        AkUGCInputBudget::MaxCopilotRequestBytes,
        TEXT("Copilot request"),
        nullptr);
    const bool bApiKeyValid = ApiKey.IsEmpty()
        || AkUGCInputBudget::ValidateUtf8Text(
            ApiKey,
            AkUGCInputBudget::MaxCopilotApiKeyBytes,
            TEXT("Copilot API key"),
            nullptr);
    bool bUrlCharactersValid = true;
    for (const TCHAR Character : Url)
    {
        if (Character <= 32 || Character >= 127)
        {
            bUrlCharactersValid = false;
            break;
        }
    }
    if (!bInitialized || !FMath::IsFinite(TimeoutSeconds) || TimeoutSeconds <= 0 || TimeoutSeconds > 60
        || !bJsonValid || !bApiKeyValid
        || Url.IsEmpty() || Url.Len() > AkUGCInputBudget::MaxCopilotUrlCharacters
        || !bUrlCharactersValid
        || Url.Contains(TEXT("\r")) || Url.Contains(TEXT("\n"))
        || Url.Contains(TEXT("@")) || Url.Contains(TEXT("\\"))
        || Url.Contains(TEXT("#")) || Url.Contains(TEXT("?"))
        || ApiKey.Contains(TEXT("\r")) || ApiKey.Contains(TEXT("\n"))
        || TestCaBundle.Len() > 4096
        || (!Url.StartsWith(TEXT("https://"), ESearchCase::CaseSensitive) && !Url.StartsWith(TEXT("http://"), ESearchCase::CaseSensitive))
        || (!ApiKey.IsEmpty() && !Url.StartsWith(TEXT("https://"), ESearchCase::CaseSensitive))) { return nullptr; }
    for (int32 Index = Workers.Num() - 1; Index >= 0; --Index)
    {
        const auto Existing = Requests[Index].Pin();
        if (!Existing.IsValid() || Existing->IsDone())
        {
            if (Workers[Index]->joinable()) { Workers[Index]->join(); }
            Workers.RemoveAt(Index); Requests.RemoveAt(Index);
        }
    }
    if (Workers.Num() >= 4) { return nullptr; }
    const auto Request = MakeShared<FAkUGCCopilotRequest, ESPMode::ThreadSafe>();
    Requests.Add(Request);
    Workers.Add(MakeUnique<std::thread>([Request, Url, Json, ApiKey, TimeoutSeconds, TestCaBundle]
    {
        FReceive Receive;
        Receive.Request = Request;
        Receive.Result.Body.Reserve(MaxBodyBytes);
        FTCHARToUTF8 UrlBytes(*Url), JsonBytes(*Json), KeyBytes(*ApiKey), CaBytes(*TestCaBundle);
        CURL* Handle = curl_easy_init();
        curl_slist* Headers = nullptr;
        CURLcode Code = Handle ? CURLE_OK : CURLE_FAILED_INIT;
        if (Handle)
        {
            const auto Set = [&](CURLoption Option, auto Value)
            { if (Code == CURLE_OK) { Code = curl_easy_setopt(Handle, Option, Value); } };
            Headers = curl_slist_append(nullptr, "Content-Type: application/json");
            if (!ApiKey.IsEmpty())
            {
                const std::string Authorization = std::string("Authorization: Bearer ") + KeyBytes.Get();
                curl_slist* Added = curl_slist_append(Headers, Authorization.c_str());
                if (Added) { Headers = Added; } else { Code = CURLE_OUT_OF_MEMORY; }
            }
            if (!Headers) { Code = CURLE_OUT_OF_MEMORY; }
            Set(CURLOPT_URL, UrlBytes.Get());
            Set(CURLOPT_PROTOCOLS_STR, ApiKey.IsEmpty() ? "http,https" : "https");
            Set(CURLOPT_FOLLOWLOCATION, 0L);
            Set(CURLOPT_MAXREDIRS, 0L);
            Set(CURLOPT_UNRESTRICTED_AUTH, 0L);
            Set(CURLOPT_PROXY, "");
            Set(CURLOPT_NETRC, static_cast<long>(CURL_NETRC_IGNORED));
            Set(CURLOPT_NOSIGNAL, 1L);
            Set(CURLOPT_SSL_VERIFYPEER, 1L);
            Set(CURLOPT_SSL_VERIFYHOST, 2L);
            Set(CURLOPT_POST, 1L);
            Set(CURLOPT_POSTFIELDS, JsonBytes.Get());
            Set(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(JsonBytes.Length()));
            Set(CURLOPT_HTTPHEADER, Headers);
            Set(CURLOPT_TIMEOUT_MS, static_cast<long>(FMath::Max(1.0f, TimeoutSeconds * 1000.0f)));
            Set(CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(FMath::Min(10000.0f, FMath::Max(1.0f, TimeoutSeconds * 1000.0f))));
            Set(CURLOPT_WRITEFUNCTION, &Write);
            Set(CURLOPT_WRITEDATA, &Receive);
            Set(CURLOPT_NOPROGRESS, 0L);
            Set(CURLOPT_XFERINFOFUNCTION, +[](void* User, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int
                { return static_cast<FAkUGCCopilotRequest*>(User)->bCancel.Load() ? 1 : 0; });
            Set(CURLOPT_XFERINFODATA, &Request.Get());
            Set(CURLOPT_ACCEPT_ENCODING, "");
            if (Url.StartsWith(TEXT("https://")))
            {
#if WITH_DEV_AUTOMATION_TESTS
                if (!TestCaBundle.IsEmpty()) { Set(CURLOPT_CAINFO, CaBytes.Get()); }
                else
#endif
                {
                    Set(CURLOPT_CAINFO, static_cast<const char*>(nullptr));
                    Set(CURLOPT_SSL_CTX_FUNCTION, &ConfigureSsl);
                }
            }
            if (Code == CURLE_OK) { Code = curl_easy_perform(Handle); }
            long Status = 0;
            curl_easy_getinfo(Handle, CURLINFO_RESPONSE_CODE, &Status);
            Receive.Result.StatusCode = static_cast<int32>(Status);
            curl_easy_cleanup(Handle);
            curl_slist_free_all(Headers);
        }
        Receive.Result.NativeCode = static_cast<int32>(Code);
        Receive.Result.bTimedOut = Code == CURLE_OPERATION_TIMEDOUT;
        Receive.Result.bCancelled = Request->bCancel.Load();
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

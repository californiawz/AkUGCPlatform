#include "Copilot/AkUGCCopilotTransport.h"

#include "Containers/StringConv.h"

bool FAkUGCCopilotTransport::Initialize()
{
    return FAkUGCEditorHttpTransport::Initialize();
}

void FAkUGCCopilotTransport::Shutdown()
{
    FAkUGCEditorHttpTransport::Shutdown();
}

FAkUGCCopilotRequestPtr FAkUGCCopilotTransport::Start(const FString& Url, const FString& Json,
    const FString& ApiKey, float TimeoutSeconds, const FString& TestCaBundle)
{
    check(IsInGameThread());
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
    if (!FMath::IsFinite(TimeoutSeconds) || TimeoutSeconds <= 0 || TimeoutSeconds > 60
        || !bJsonValid || !bApiKeyValid
        || TestCaBundle.Len() > 4096
        || (!ApiKey.IsEmpty()
            && !Url.StartsWith(TEXT("https://"), ESearchCase::CaseSensitive)))
    {
        return nullptr;
    }

    FTCHARToUTF8 JsonBytes(*Json);
    FAkUGCEditorHttpRequestSpec Spec;
    Spec.Method = EAkUGCEditorHttpMethod::Post;
    Spec.Url = Url;
    Spec.BearerToken = ApiKey;
    Spec.ContentType = TEXT("application/json");
    Spec.TimeoutSeconds = TimeoutSeconds;
    Spec.MaxResponseBytes = MaxBodyBytes;
    Spec.Body.Append(
        reinterpret_cast<const uint8*>(JsonBytes.Get()),
        JsonBytes.Length());
#if WITH_DEV_AUTOMATION_TESTS
    Spec.TestCaBundle = TestCaBundle;
#endif
    return FAkUGCEditorHttpTransport::Start(MoveTemp(Spec));
}

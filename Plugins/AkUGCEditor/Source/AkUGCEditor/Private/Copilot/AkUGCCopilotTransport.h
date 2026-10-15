#pragma once

#include "Transport/AkUGCEditorHttpTransport.h"
#include "Validation/AkUGCInputBudget.h"

using FAkUGCCopilotTransportResult = FAkUGCEditorHttpResult;
using FAkUGCCopilotRequest = FAkUGCEditorHttpRequest;
using FAkUGCCopilotRequestPtr = FAkUGCEditorHttpRequestPtr;

class FAkUGCCopilotTransport
{
public:
    static bool Initialize();
    static void Shutdown();
    static FAkUGCCopilotRequestPtr Start(const FString& Url, const FString& Json,
        const FString& ApiKey, float TimeoutSeconds, const FString& TestCaBundle = FString());
    static constexpr int32 MaxBodyBytes =
        static_cast<int32>(AkUGCInputBudget::MaxCopilotResponseBytes);
};

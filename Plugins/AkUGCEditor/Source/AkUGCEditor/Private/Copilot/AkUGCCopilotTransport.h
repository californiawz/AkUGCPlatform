#pragma once

#include "CoreMinimal.h"
#include "Validation/AkUGCInputBudget.h"

struct FAkUGCCopilotTransportResult
{
    int32 NativeCode = 0;
    int32 StatusCode = 0;
    bool bTimedOut = false;
    bool bCancelled = false;
    bool bTooLarge = false;
    TArray<uint8> Body;
};

class FAkUGCCopilotRequest : public TSharedFromThis<FAkUGCCopilotRequest, ESPMode::ThreadSafe>
{
public:
    void Cancel() { bCancel.Store(true); }
    bool IsDone() const { return bDone.Load(); }
    bool IsCancelled() const { return bCancel.Load(); }
    FAkUGCCopilotTransportResult TakeResult();
private:
    friend class FAkUGCCopilotTransport;
    TAtomic<bool> bCancel{false};
    TAtomic<bool> bDone{false};
    FCriticalSection Mutex;
    FAkUGCCopilotTransportResult Result;
};

using FAkUGCCopilotRequestPtr = TSharedPtr<FAkUGCCopilotRequest, ESPMode::ThreadSafe>;

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

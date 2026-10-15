#pragma once

#include "CoreMinimal.h"

enum class EAkUGCEditorHttpMethod : uint8
{
	Get,
	Post
};

struct FAkUGCEditorHttpRequestSpec
{
	EAkUGCEditorHttpMethod Method = EAkUGCEditorHttpMethod::Get;
	FString Url;
	FString BearerToken;
	FString ContentType;
	FString IdempotencyKey;
	TArray<uint8> Body;
	float TimeoutSeconds = 60.0f;
	int32 MaxResponseBytes = 256 * 1024;
	bool bAllowLoopbackHttpWithBearer = false;
#if WITH_DEV_AUTOMATION_TESTS
	FString TestCaBundle;
#endif
};

struct FAkUGCEditorHttpResult
{
	int32 NativeCode = 0;
	int32 StatusCode = 0;
	bool bTimedOut = false;
	bool bCancelled = false;
	bool bTooLarge = false;
	TArray<uint8> Body;
};

class FAkUGCEditorHttpRequest
	: public TSharedFromThis<FAkUGCEditorHttpRequest, ESPMode::ThreadSafe>
{
public:
	void Cancel() { bCancel.Store(true); }
	bool IsDone() const { return bDone.Load(); }
	bool IsCancelled() const { return bCancel.Load(); }
	FAkUGCEditorHttpResult TakeResult();

private:
	friend class FAkUGCEditorHttpTransport;
	TAtomic<bool> bCancel{false};
	TAtomic<bool> bDone{false};
	FCriticalSection Mutex;
	FAkUGCEditorHttpResult Result;
};

using FAkUGCEditorHttpRequestPtr =
	TSharedPtr<FAkUGCEditorHttpRequest, ESPMode::ThreadSafe>;

class FAkUGCEditorHttpTransport
{
public:
	static bool Initialize();
	static void Shutdown();
	static FAkUGCEditorHttpRequestPtr Start(FAkUGCEditorHttpRequestSpec Spec);
};

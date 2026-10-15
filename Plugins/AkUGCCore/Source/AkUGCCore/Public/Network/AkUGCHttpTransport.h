#pragma once

#include "CoreMinimal.h"

enum class EAkUGCHttpMethod : uint8
{
	Get,
	Post
};

struct AKUGCCORE_API FAkUGCHttpHeader
{
	FString Name;
	FString Value;
};

struct AKUGCCORE_API FAkUGCHttpRequestSpec
{
	EAkUGCHttpMethod Method = EAkUGCHttpMethod::Get;
	FString Url;
	FString BearerToken;
	FString ContentType;
	FString IdempotencyKey;
	TArray<uint8> Body;
	float TimeoutSeconds = 60.0f;
	int32 MaxResponseBytes = 256 * 1024;
	int32 MaxResponseHeaderBytes = 64 * 1024;
	bool bAllowLoopbackHttpWithBearer = false;
	bool bAcceptCompressedResponse = true;
#if WITH_DEV_AUTOMATION_TESTS
	FString TestCaBundle;
#endif
};

struct AKUGCCORE_API FAkUGCHttpResult
{
	int32 NativeCode = 0;
	int32 StatusCode = 0;
	bool bTimedOut = false;
	bool bCancelled = false;
	bool bTooLarge = false;
	bool bHeadersTooLarge = false;
	FString EffectiveUrl;
	TArray<FAkUGCHttpHeader> Headers;
	TArray<uint8> Body;
};

class AKUGCCORE_API FAkUGCHttpRequest
	: public TSharedFromThis<FAkUGCHttpRequest, ESPMode::ThreadSafe>
{
public:
	void Cancel() { bCancel.Store(true); }
	bool IsDone() const { return bDone.Load(); }
	bool IsCancelled() const { return bCancel.Load(); }
	FAkUGCHttpResult TakeResult();

private:
	friend class FAkUGCHttpTransport;
	TAtomic<bool> bCancel{false};
	TAtomic<bool> bDone{false};
	FCriticalSection Mutex;
	FAkUGCHttpResult Result;
};

using FAkUGCHttpRequestPtr =
	TSharedPtr<FAkUGCHttpRequest, ESPMode::ThreadSafe>;

class AKUGCCORE_API FAkUGCHttpTransport
{
public:
	/**
	 * Initialization is reference counted so dependent modules can share one
	 * libcurl lifecycle. Calls must be made on the game thread.
	 */
	static bool Initialize();
	static void Shutdown();
	static FAkUGCHttpRequestPtr Start(FAkUGCHttpRequestSpec Spec);
};

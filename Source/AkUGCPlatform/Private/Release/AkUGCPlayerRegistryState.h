#pragma once

#include "CoreMinimal.h"
#include "Network/AkUGCHttpTransport.h"

struct FAkUGCPlayerRegistryState
{
	FAkUGCHttpRequestPtr Request;
	FGuid ExpectedReleaseId;
	FString ExpectedUrl;
	FString IndependentlyTrustedPublicKey;
	FString FailureCode;
};

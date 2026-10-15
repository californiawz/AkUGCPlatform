#pragma once

#include "CoreMinimal.h"
#include "Network/AkUGCHttpTransport.h"

enum class EAkUGCPlayerRegistryStage : uint8
{
	None,
	WorkPage,
	Download
};

struct FAkUGCPlayerRegistryState
{
	~FAkUGCPlayerRegistryState()
	{
		TArray<TCHAR>& Characters = BearerToken.GetCharArray();
		if (!Characters.IsEmpty())
		{
			FMemory::Memzero(
				Characters.GetData(),
				Characters.Num() * sizeof(TCHAR));
		}
	}

	FAkUGCHttpRequestPtr Request;
	EAkUGCPlayerRegistryStage Stage =
		EAkUGCPlayerRegistryStage::None;
	FGuid ExpectedProjectId;
	FGuid ExpectedReleaseId;
	FString ExpectedUrl;
	FString RegistryBaseUrl;
	FString BearerToken;
	FString IndependentlyTrustedPublicKey;
	FString FailureCode;
};

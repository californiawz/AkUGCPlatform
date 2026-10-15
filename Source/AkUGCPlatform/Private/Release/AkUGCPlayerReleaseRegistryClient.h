#pragma once

#include "CoreMinimal.h"
#include "Network/AkUGCHttpTransport.h"
#include "Pack/AkUGCWorkLibrary.h"

class FAkUGCPlayerReleaseRegistryClient
{
public:
	static constexpr int32 MaxArtifactBytes =
		static_cast<int32>(FAkUGCWorkLibrary::MaxFileBytes);
	static constexpr int32 MaxResponseHeaderBytes = 64 * 1024;

	static FAkUGCHttpRequestPtr StartDownload(
		const FString& BaseUrl,
		const FString& BearerToken,
		const FGuid& ReleaseId,
		float TimeoutSeconds,
		FString& OutExpectedUrl,
		FString& OutError);

	static bool ImportDownloaded(
		const FAkUGCHttpResult& Transport,
		const FString& ExpectedUrl,
		const FGuid& ExpectedReleaseId,
		const FString& IndependentlyTrustedPublicKey,
		const FString& LibraryDirectory,
		FAkUGCWorkEntry& OutEntry,
		FString& OutArtifactSha256,
		FString& OutFailureCode,
		FString& OutError);

	static bool NormalizeBaseUrl(
		const FString& BaseUrl,
		FString& OutNormalized,
		FString& OutError);

	static bool ValidateBearerToken(
		const FString& BearerToken,
		FString& OutError);

	static bool NormalizeTrustedPublicKey(
		const FString& PublicKey,
		FString& OutNormalized,
		FString& OutError);
};

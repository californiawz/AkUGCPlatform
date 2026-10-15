#pragma once

#include "CoreMinimal.h"
#include "Pack/AkUGCWorkLibrary.h"
#include "Transport/AkUGCEditorHttpTransport.h"

enum class EAkUGCReleaseRegistryOperation : uint8
{
	Submit,
	Get,
	Publish
};

struct FAkUGCReleaseRegistryView
{
	FGuid ReleaseId;
	FString Status;
	FString OwnerSubject;
	FString PublisherPublicKey;
	FString ContentHash;
	FString ArtifactSha256;
	int64 Revision = 0;
};

struct FAkUGCReleaseRegistryResponse
{
	bool bCreated = false;
	bool bChanged = false;
	FAkUGCReleaseRegistryView Release;
};

class FAkUGCReleaseRegistryClient
{
public:
	static constexpr int32 MaxControlResponseBytes = 64 * 1024;

	static FAkUGCEditorHttpRequestPtr StartSubmit(
		const FString& BaseUrl,
		const FString& BearerToken,
		const FAkUGCWorkEntry& Entry,
		const FString& SharePath,
		float TimeoutSeconds,
		FString& OutError);

	static FAkUGCEditorHttpRequestPtr StartGet(
		const FString& BaseUrl,
		const FString& BearerToken,
		const FGuid& ReleaseId,
		float TimeoutSeconds,
		FString& OutError);

	static FAkUGCEditorHttpRequestPtr StartPublish(
		const FString& BaseUrl,
		const FString& BearerToken,
		const FGuid& ReleaseId,
		float TimeoutSeconds,
		FString& OutError);

	static bool ParseResponse(
		EAkUGCReleaseRegistryOperation Operation,
		const FAkUGCEditorHttpResult& Transport,
		const FAkUGCWorkEntry& ExpectedEntry,
		FAkUGCReleaseRegistryResponse& OutResponse,
		FString& OutError);

	static bool NormalizeBaseUrl(
		const FString& BaseUrl,
		FString& OutNormalized,
		FString& OutError);

	static bool ValidateBearerToken(
		const FString& BearerToken,
		FString& OutError);
};

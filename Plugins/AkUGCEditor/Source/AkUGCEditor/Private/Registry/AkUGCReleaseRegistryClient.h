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

enum class EAkUGCProjectRegistryOperation : uint8
{
	CreateProject,
	GetProject,
	ListMembers,
	SetMember,
	RemoveMember,
	ListVersions,
	SetVisibility
};

struct FAkUGCReleaseRegistryView
{
	FGuid ReleaseId;
	FGuid ProjectId;
	FString Status;
	FString OwnerSubject;
	FString SubmittedBySubject;
	FString PublisherPublicKey;
	FString ContentHash;
	FString ArtifactSha256;
	FString SubmittedAt;
	int64 Revision = 0;
};

struct FAkUGCReleaseRegistryResponse
{
	bool bCreated = false;
	bool bChanged = false;
	FAkUGCReleaseRegistryView Release;
};

struct FAkUGCRegistryProjectView
{
	FGuid ProjectId;
	FString DisplayName;
	FString OwnerSubject;
	FString Visibility;
	FString CreatedAt;
	FString UpdatedAt;
	int64 Revision = 0;
};

struct FAkUGCRegistryMemberView
{
	FGuid ProjectId;
	FString Subject;
	FString Role;
	FString AddedAt;
	FString UpdatedAt;
};

struct FAkUGCProjectRegistryResponse
{
	bool bCreated = false;
	bool bChanged = false;
	FString AccessRole;
	FAkUGCRegistryProjectView Project;
	FAkUGCRegistryMemberView Member;
	TArray<FAkUGCRegistryMemberView> Members;
	TArray<FAkUGCReleaseRegistryView> Versions;
	FString NextCursor;
};

class FAkUGCReleaseRegistryClient
{
public:
	static constexpr int32 MaxControlResponseBytes = 64 * 1024;
	static constexpr int32 MaxProjectMembers = 256;
	static constexpr int32 MaxVersionsPerPage = 100;

	static bool ValidateSubmissionArtifact(
		const FAkUGCWorkEntry& Entry,
		const FString& SharePath,
		const FGuid& ExpectedProjectId,
		FString& OutError);

	static FAkUGCEditorHttpRequestPtr StartSubmit(
		const FString& BaseUrl,
		const FString& BearerToken,
		const FAkUGCWorkEntry& Entry,
		const FString& SharePath,
		const FGuid& ExpectedProjectId,
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

	static FAkUGCEditorHttpRequestPtr StartCreateProject(
		const FString& BaseUrl,
		const FString& BearerToken,
		const FGuid& ProjectId,
		const FString& DisplayName,
		float TimeoutSeconds,
		FString& OutError);

	static FAkUGCEditorHttpRequestPtr StartGetProject(
		const FString& BaseUrl,
		const FString& BearerToken,
		const FGuid& ProjectId,
		float TimeoutSeconds,
		FString& OutError);

	static FAkUGCEditorHttpRequestPtr StartListProjectMembers(
		const FString& BaseUrl,
		const FString& BearerToken,
		const FGuid& ProjectId,
		float TimeoutSeconds,
		FString& OutError);

	static FAkUGCEditorHttpRequestPtr StartSetProjectMember(
		const FString& BaseUrl,
		const FString& BearerToken,
		const FGuid& ProjectId,
		const FString& Subject,
		const FString& Role,
		float TimeoutSeconds,
		FString& OutError);

	static FAkUGCEditorHttpRequestPtr StartRemoveProjectMember(
		const FString& BaseUrl,
		const FString& BearerToken,
		const FGuid& ProjectId,
		const FString& Subject,
		float TimeoutSeconds,
		FString& OutError);

	static FAkUGCEditorHttpRequestPtr StartListProjectVersions(
		const FString& BaseUrl,
		const FString& BearerToken,
		const FGuid& ProjectId,
		int32 Limit,
		float TimeoutSeconds,
		FString& OutError);

	static FAkUGCEditorHttpRequestPtr StartSetProjectVisibility(
		const FString& BaseUrl,
		const FString& BearerToken,
		const FGuid& ProjectId,
		const FString& Visibility,
		float TimeoutSeconds,
		FString& OutError);

	static bool ParseResponse(
		EAkUGCReleaseRegistryOperation Operation,
		const FAkUGCEditorHttpResult& Transport,
		const FAkUGCWorkEntry& ExpectedEntry,
		const FGuid& ExpectedProjectId,
		FAkUGCReleaseRegistryResponse& OutResponse,
		FString& OutError);

	static bool ParseProjectResponse(
		EAkUGCProjectRegistryOperation Operation,
		const FAkUGCEditorHttpResult& Transport,
		const FGuid& ExpectedProjectId,
		FAkUGCProjectRegistryResponse& OutResponse,
		FString& OutError);

	static bool NormalizeBaseUrl(
		const FString& BaseUrl,
		FString& OutNormalized,
		FString& OutError);

	static bool ValidateBearerToken(
		const FString& BearerToken,
		FString& OutError);

	static bool ValidateProjectDisplayName(
		const FString& DisplayName,
		FString& OutError);

	static bool ValidateMemberSubject(
		const FString& Subject,
		FString& OutError);
};

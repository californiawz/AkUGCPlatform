#include "Misc/AutomationTest.h"

#include "Containers/StringConv.h"
#include "Editor.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Pack/AkUGCLogicPackHasher.h"
#include "Pack/AkUGCLogicPackSignature.h"
#include "Registry/AkUGCReleaseRegistryClient.h"
#include "Subsystem/AkUGCEditorSubsystem.h"
#include "Widgets/SAkUGCWorkLibraryPanel.h"

namespace AkUGCReleaseRegistryTest
{
	FGuid ExpectedProjectId()
	{
		FGuid ProjectId;
		FGuid::Parse(
			TEXT("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"),
			ProjectId);
		return ProjectId;
	}

	FAkUGCWorkEntry ExpectedEntry()
	{
		FAkUGCWorkEntry Entry;
		FGuid::Parse(
			TEXT("11111111-2222-3333-4444-555555555555"),
			Entry.ReleaseId);
		Entry.TrustedPublicKey = FString::ChrN(64, TEXT('a'));
		Entry.ContentHash = FString::ChrN(64, TEXT('b'));
		return Entry;
	}

	FString ReleaseJson(const TCHAR* Status)
	{
		return FString::Printf(
			TEXT("{")
			TEXT("\"releaseId\":\"11111111-2222-3333-4444-555555555555\",")
			TEXT("\"projectId\":\"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee\",")
			TEXT("\"schemaVersion\":6,")
			TEXT("\"ownerSubject\":\"creator-a\",")
			TEXT("\"submittedBySubject\":\"creator-a\",")
			TEXT("\"publisherPublicKey\":\"%s\",")
			TEXT("\"contentHash\":\"%s\",")
			TEXT("\"artifactSha256\":\"%s\",")
			TEXT("\"artifactSize\":1024,")
			TEXT("\"status\":\"%s\",")
			TEXT("\"revision\":2,")
			TEXT("\"submittedAt\":\"2026-10-15T06:00:00Z\",")
			TEXT("\"updatedAt\":\"2026-10-15T06:01:00Z\"")
			TEXT("}"),
			*FString::ChrN(64, TEXT('a')),
			*FString::ChrN(64, TEXT('b')),
			*FString::ChrN(64, TEXT('c')),
			Status);
	}

	FString ProjectJson(int64 Revision = 1)
	{
		return FString::Printf(
			TEXT("{")
			TEXT("\"projectId\":\"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee\",")
			TEXT("\"displayName\":\"Creator Project\",")
			TEXT("\"ownerSubject\":\"creator-a\",")
			TEXT("\"revision\":%lld,")
			TEXT("\"createdAt\":\"2026-10-15T06:00:00Z\",")
			TEXT("\"updatedAt\":\"2026-10-15T06:01:00Z\"")
			TEXT("}"),
			static_cast<long long>(Revision));
	}

	FString MemberJson(const TCHAR* Subject, const TCHAR* Role)
	{
		return FString::Printf(
			TEXT("{")
			TEXT("\"projectId\":\"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee\",")
			TEXT("\"subject\":\"%s\",")
			TEXT("\"role\":\"%s\",")
			TEXT("\"addedAt\":\"2026-10-15T06:00:00Z\",")
			TEXT("\"updatedAt\":\"2026-10-15T06:01:00Z\"")
			TEXT("}"),
			Subject,
			Role);
	}

	FAkUGCEditorHttpResult Result(int32 StatusCode, const FString& Json)
	{
		FAkUGCEditorHttpResult Result;
		Result.StatusCode = StatusCode;
		FTCHARToUTF8 Bytes(*Json);
		Result.Body.Append(
			reinterpret_cast<const uint8*>(Bytes.Get()),
			Bytes.Length());
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAkUGCReleaseRegistryConfigurationTest,
	"AkUGC.Editor.ReleaseRegistry.ConfigurationBoundaries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCReleaseRegistryConfigurationTest::RunTest(const FString& Parameters)
{
	FString Normalized;
	FString Error;
	TestTrue(
		TEXT("IPv4 loopback base is accepted"),
		FAkUGCReleaseRegistryClient::NormalizeBaseUrl(
			TEXT("http://127.0.0.1:8088/"),
			Normalized,
			Error));
	TestEqual(
		TEXT("Trailing slash is normalized"),
		Normalized,
		FString(TEXT("http://127.0.0.1:8088")));
	TestTrue(
		TEXT("localhost base is accepted"),
		FAkUGCReleaseRegistryClient::NormalizeBaseUrl(
			TEXT("http://localhost:49152"),
			Normalized,
			Error));

	for (const FString& Invalid : {
		TEXT("https://127.0.0.1:8088"),
		TEXT("http://0.0.0.0:8088"),
		TEXT("http://127.0.0.1.evil:8088"),
		TEXT("http://127.0.0.1:0"),
		TEXT("http://127.0.0.1:65536"),
		TEXT("http://127.0.0.1:8088/path"),
		TEXT("http://user@127.0.0.1:8088"),
		TEXT("http://127.0.0.1:8088?token=leak")})
	{
		TestFalse(
			*FString::Printf(TEXT("Invalid base is rejected: %s"), *Invalid),
			FAkUGCReleaseRegistryClient::NormalizeBaseUrl(
				Invalid,
				Normalized,
				Error));
	}
	TestTrue(
		TEXT("Visible ASCII token is accepted"),
		FAkUGCReleaseRegistryClient::ValidateBearerToken(
			TEXT("creator-token_123456789"),
			Error));
	TestFalse(
		TEXT("Empty token is rejected"),
		FAkUGCReleaseRegistryClient::ValidateBearerToken(FString(), Error));
	TestFalse(
		TEXT("Header injection token is rejected"),
		FAkUGCReleaseRegistryClient::ValidateBearerToken(
			TEXT("creator\r\nX-Injected: yes"),
			Error));
	TestFalse(
		TEXT("Non-ASCII token is rejected"),
		FAkUGCReleaseRegistryClient::ValidateBearerToken(
			TEXT("creator-令牌"),
			Error));
	TestTrue(
		TEXT("UTF-8 project display name is accepted"),
		FAkUGCReleaseRegistryClient::ValidateProjectDisplayName(
			TEXT("塔防创作项目"),
			Error));
	TestFalse(
		TEXT("Project display name with surrounding whitespace is rejected"),
		FAkUGCReleaseRegistryClient::ValidateProjectDisplayName(
			TEXT(" Creator Project"),
			Error));
	TestTrue(
		TEXT("Safe collaborator subject is accepted"),
		FAkUGCReleaseRegistryClient::ValidateMemberSubject(
			TEXT("team.editor-1"),
			Error));
	TestFalse(
		TEXT("Collaborator subject containing a slash is rejected"),
		FAkUGCReleaseRegistryClient::ValidateMemberSubject(
			TEXT("team/editor"),
			Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAkUGCReleaseRegistryResponseTest,
	"AkUGC.Editor.ReleaseRegistry.StrictResponseParsing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCReleaseRegistryResponseTest::RunTest(const FString& Parameters)
{
	using namespace AkUGCReleaseRegistryTest;
	const FAkUGCWorkEntry Expected = ExpectedEntry();
	FAkUGCReleaseRegistryResponse Response;
	FString Error;

	const FString Submitted =
		TEXT("{\"created\":true,\"release\":")
		+ ReleaseJson(TEXT("submitted"))
		+ TEXT("}");
	TestTrue(
		TEXT("Created submit response parses"),
		FAkUGCReleaseRegistryClient::ParseResponse(
			EAkUGCReleaseRegistryOperation::Submit,
			Result(201, Submitted),
			Expected,
			ExpectedProjectId(),
			Response,
			Error));
	TestTrue(TEXT("Created flag is preserved"), Response.bCreated);
	TestEqual(
		TEXT("Submitted status is preserved"),
		Response.Release.Status,
		FString(TEXT("submitted")));
	TestEqual(
		TEXT("Actual submitter is preserved"),
		Response.Release.SubmittedBySubject,
		FString(TEXT("creator-a")));

	const FString Approved =
		TEXT("{\"release\":")
		+ ReleaseJson(TEXT("approved"))
		+ TEXT("}");
	TestTrue(
		TEXT("Approved get response parses"),
		FAkUGCReleaseRegistryClient::ParseResponse(
			EAkUGCReleaseRegistryOperation::Get,
			Result(200, Approved),
			Expected,
			ExpectedProjectId(),
			Response,
			Error));
	TestEqual(
		TEXT("Approved status is preserved"),
		Response.Release.Status,
		FString(TEXT("approved")));

	const FString Published =
		TEXT("{\"changed\":true,\"release\":")
		+ ReleaseJson(TEXT("published"))
		+ TEXT("}");
	TestTrue(
		TEXT("Published transition response parses"),
		FAkUGCReleaseRegistryClient::ParseResponse(
			EAkUGCReleaseRegistryOperation::Publish,
			Result(200, Published),
			Expected,
			ExpectedProjectId(),
			Response,
			Error));
	TestTrue(TEXT("Changed flag is preserved"), Response.bChanged);

	const FString Duplicate =
		TEXT("{\"release\":")
		+ ReleaseJson(TEXT("approved"))
		+ TEXT(",\"release\":")
		+ ReleaseJson(TEXT("rejected"))
		+ TEXT("}");
	TestFalse(
		TEXT("Duplicate response field is rejected"),
		FAkUGCReleaseRegistryClient::ParseResponse(
			EAkUGCReleaseRegistryOperation::Get,
			Result(200, Duplicate),
			Expected,
			ExpectedProjectId(),
			Response,
			Error));

	const FString Unknown =
		TEXT("{\"release\":")
		+ ReleaseJson(TEXT("approved"))
		+ TEXT(",\"trustedByServer\":true}");
	TestFalse(
		TEXT("Unknown response field is rejected"),
		FAkUGCReleaseRegistryClient::ParseResponse(
			EAkUGCReleaseRegistryOperation::Get,
			Result(200, Unknown),
			Expected,
			ExpectedProjectId(),
			Response,
			Error));

	FString WrongKeyRelease = ReleaseJson(TEXT("approved"));
	WrongKeyRelease.ReplaceInline(
		*FString::ChrN(64, TEXT('a')),
		*FString::ChrN(64, TEXT('d')),
		ESearchCase::CaseSensitive);
	TestFalse(
		TEXT("Server key cannot replace the local publisher key"),
		FAkUGCReleaseRegistryClient::ParseResponse(
			EAkUGCReleaseRegistryOperation::Get,
			Result(
				200,
				TEXT("{\"release\":") + WrongKeyRelease + TEXT("}")),
			Expected,
			ExpectedProjectId(),
			Response,
			Error));

	FAkUGCEditorHttpResult TooLarge;
	TooLarge.bTooLarge = true;
	TestFalse(
		TEXT("Receive-time response overflow is rejected"),
		FAkUGCReleaseRegistryClient::ParseResponse(
			EAkUGCReleaseRegistryOperation::Get,
			TooLarge,
			Expected,
			ExpectedProjectId(),
			Response,
			Error));

	FAkUGCEditorHttpResult Cancelled;
	Cancelled.bCancelled = true;
	TestFalse(
		TEXT("Cancelled response is rejected"),
		FAkUGCReleaseRegistryClient::ParseResponse(
			EAkUGCReleaseRegistryOperation::Get,
			Cancelled,
			Expected,
			ExpectedProjectId(),
			Response,
			Error));

	FAkUGCEditorHttpResult InvalidUtf8;
	InvalidUtf8.StatusCode = 200;
	InvalidUtf8.Body = {0xff, 0xfe};
	TestFalse(
		TEXT("Invalid UTF-8 response is rejected"),
		FAkUGCReleaseRegistryClient::ParseResponse(
			EAkUGCReleaseRegistryOperation::Get,
			InvalidUtf8,
			Expected,
			ExpectedProjectId(),
			Response,
			Error));

	TestFalse(
		TEXT("Unexpected HTTP status is rejected"),
		FAkUGCReleaseRegistryClient::ParseResponse(
			EAkUGCReleaseRegistryOperation::Get,
			Result(404, TEXT("{}")),
			Expected,
			ExpectedProjectId(),
			Response,
			Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAkUGCProjectRegistryResponseTest,
	"AkUGC.Editor.ReleaseRegistry.ProjectStrictResponseParsing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCProjectRegistryResponseTest::RunTest(const FString& Parameters)
{
	using namespace AkUGCReleaseRegistryTest;
	FAkUGCProjectRegistryResponse Response;
	FString Error;
	const FGuid ProjectId = ExpectedProjectId();

	TestTrue(
		TEXT("Project response preserves current access role"),
		FAkUGCReleaseRegistryClient::ParseProjectResponse(
			EAkUGCProjectRegistryOperation::GetProject,
			Result(
				200,
				TEXT("{\"accessRole\":\"editor\",\"project\":")
					+ ProjectJson()
					+ TEXT("}")),
			ProjectId,
			Response,
			Error));
	TestEqual(
		TEXT("Project access role is editor"),
		Response.AccessRole,
		FString(TEXT("editor")));
	TestEqual(
		TEXT("Project ID remains bound to the current document"),
		Response.Project.ProjectId,
		ProjectId);

	TestTrue(
		TEXT("Member list response parses"),
		FAkUGCReleaseRegistryClient::ParseProjectResponse(
			EAkUGCProjectRegistryOperation::ListMembers,
			Result(
				200,
				TEXT("{\"members\":[")
					+ MemberJson(TEXT("creator-a"), TEXT("owner"))
					+ TEXT(",")
					+ MemberJson(TEXT("editor-a"), TEXT("editor"))
					+ TEXT(",")
					+ MemberJson(TEXT("viewer-a"), TEXT("viewer"))
					+ TEXT("]}")),
			ProjectId,
			Response,
			Error));
	TestEqual(
		TEXT("Owner and both collaborators are preserved"),
		Response.Members.Num(),
		3);
	TestFalse(
		TEXT("Member list without exactly one owner is rejected"),
		FAkUGCReleaseRegistryClient::ParseProjectResponse(
			EAkUGCProjectRegistryOperation::ListMembers,
			Result(
				200,
				TEXT("{\"members\":[")
					+ MemberJson(TEXT("editor-a"), TEXT("editor"))
					+ TEXT("]}")),
			ProjectId,
			Response,
			Error));

	TestTrue(
		TEXT("Version list response parses with truncation cursor"),
		FAkUGCReleaseRegistryClient::ParseProjectResponse(
			EAkUGCProjectRegistryOperation::ListVersions,
			Result(
				200,
				TEXT("{\"versions\":[")
					+ ReleaseJson(TEXT("published"))
					+ TEXT("],\"nextCursor\":")
					+ TEXT("\"11111111-2222-3333-4444-555555555555\"}")),
			ProjectId,
			Response,
			Error));
	TestEqual(
		TEXT("One immutable version is preserved"),
		Response.Versions.Num(),
		1);
	TestFalse(
		TEXT("Version cursor is not silently discarded"),
		Response.NextCursor.IsEmpty());

	TestFalse(
		TEXT("Unknown Project response field is rejected"),
		FAkUGCReleaseRegistryClient::ParseProjectResponse(
			EAkUGCProjectRegistryOperation::GetProject,
			Result(
				200,
				TEXT("{\"accessRole\":\"owner\",\"project\":")
					+ ProjectJson()
					+ TEXT(",\"serverTrusted\":true}")),
			ProjectId,
			Response,
			Error));

	FString WrongProject = ProjectJson();
	WrongProject.ReplaceInline(
		TEXT("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"),
		TEXT("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeef"),
		ESearchCase::CaseSensitive);
	TestFalse(
		TEXT("Project response for another document is rejected"),
		FAkUGCReleaseRegistryClient::ParseProjectResponse(
			EAkUGCProjectRegistryOperation::GetProject,
			Result(
				200,
				TEXT("{\"accessRole\":\"owner\",\"project\":")
					+ WrongProject
					+ TEXT("}")),
			ProjectId,
			Response,
			Error));

	TestFalse(
		TEXT("Duplicate collaborator subject is rejected"),
		FAkUGCReleaseRegistryClient::ParseProjectResponse(
			EAkUGCProjectRegistryOperation::ListMembers,
			Result(
				200,
				TEXT("{\"members\":[")
					+ MemberJson(TEXT("viewer-a"), TEXT("viewer"))
					+ TEXT(",")
					+ MemberJson(TEXT("viewer-a"), TEXT("editor"))
					+ TEXT("]}")),
			ProjectId,
			Response,
			Error));

	TestFalse(
		TEXT("Malformed version cursor is rejected"),
		FAkUGCReleaseRegistryClient::ParseProjectResponse(
			EAkUGCProjectRegistryOperation::ListVersions,
			Result(
				200,
				TEXT("{\"versions\":[],\"nextCursor\":\"not-a-guid\"}")),
			ProjectId,
			Response,
			Error));
	TestFalse(
		TEXT("Version cursor not bound to the last item is rejected"),
		FAkUGCReleaseRegistryClient::ParseProjectResponse(
			EAkUGCProjectRegistryOperation::ListVersions,
			Result(
				200,
				TEXT("{\"versions\":[")
					+ ReleaseJson(TEXT("published"))
					+ TEXT("],\"nextCursor\":")
					+ TEXT("\"11111111-2222-3333-4444-555555555556\"}")),
			ProjectId,
			Response,
			Error));
	return true;
}

#if WITH_DEV_AUTOMATION_TESTS
struct FAkUGCReleaseRegistryPanelTestAccess
{
	static void Configure(
		SAkUGCWorkLibraryPanel& Panel,
		const FString& BaseUrl,
		const FString& Token,
		const FAkUGCWorkEntry& Entry,
		const FString& SharePath)
	{
		Panel.RegistryBaseUrl = BaseUrl;
		Panel.RegistryToken = Token;
		Panel.LastPublishedEntry = Entry;
		Panel.LastPublishedSharePath = SharePath;
		Panel.RegistryRequestTimeoutSeconds = 10.0f;
	}

	static void Submit(SAkUGCWorkLibraryPanel& Panel)
	{
		Panel.SubmitToRegistry();
	}

	static void Refresh(SAkUGCWorkLibraryPanel& Panel)
	{
		Panel.RefreshRegistryStatus();
	}

	static void Publish(SAkUGCWorkLibraryPanel& Panel)
	{
		Panel.PublishToRegistry();
	}

	static void RefreshProject(SAkUGCWorkLibraryPanel& Panel)
	{
		Panel.RefreshRegistryProject();
	}

	static void SetMember(
		SAkUGCWorkLibraryPanel& Panel,
		const FString& Subject,
		const FString& Role)
	{
		Panel.RegistryMemberSubject = Subject;
		Panel.RegistryMemberRole = Role;
		Panel.SetRegistryMember();
	}

	static void RemoveMember(
		SAkUGCWorkLibraryPanel& Panel,
		const FString& Subject)
	{
		Panel.RegistryMemberSubject = Subject;
		Panel.RemoveRegistryMember();
	}

	static void Poll(SAkUGCWorkLibraryPanel& Panel)
	{
		Panel.Tick(FGeometry(), FPlatformTime::Seconds(), 0.0f);
	}

	static bool Pending(const SAkUGCWorkLibraryPanel& Panel)
	{
		return Panel.PendingRegistryRequest.IsValid();
	}

	static const FString& ReleaseStatus(const SAkUGCWorkLibraryPanel& Panel)
	{
		return Panel.RegistryReleaseStatus;
	}

	static const FString& Status(const SAkUGCWorkLibraryPanel& Panel)
	{
		return Panel.RegistryStatus;
	}

	static const FString& ProjectAccessRole(
		const SAkUGCWorkLibraryPanel& Panel)
	{
		return Panel.RegistryProjectAccessRole;
	}

	static int32 MemberCount(const SAkUGCWorkLibraryPanel& Panel)
	{
		return Panel.RegistryMembers.Num();
	}

	static int32 VersionCount(const SAkUGCWorkLibraryPanel& Panel)
	{
		return Panel.RegistryVersions.Num();
	}

	static bool CheckCancellation(SAkUGCWorkLibraryPanel& Panel)
	{
		const FAkUGCEditorHttpRequestPtr Request =
			MakeShared<FAkUGCEditorHttpRequest, ESPMode::ThreadSafe>();
		Panel.PendingRegistryRequest = Request;
		Panel.PendingRegistryRequestKind =
			EAkUGCRegistryPanelRequest::EnsureProject;
		Panel.CancelRegistryRequest();
		FAkUGCEditorHttpResult Result;
		Result.bCancelled = true;
		Panel.CompleteRegistryRequest(Request, MoveTemp(Result));
		return Request->IsCancelled()
			&& !Panel.PendingRegistryRequest.IsValid()
			&& Panel.PendingRegistryRequestKind
				== EAkUGCRegistryPanelRequest::None
			&& Panel.RegistryStatus.Contains(TEXT("后续请求链已停止"));
	}
};

namespace AkUGCReleaseRegistryHttpTest
{
	class FObserve final : public IAutomationLatentCommand
	{
	public:
		FObserve(
			FAutomationTestBase* InTest,
			UAkUGCEditorSubsystem* InSubsystem,
			const TSharedRef<SAkUGCWorkLibraryPanel>& InPanel,
			const FString& InRoot,
			uint64 InRevision,
			const FString& InDocumentHash)
			: Test(InTest)
			, Subsystem(InSubsystem)
			, Panel(InPanel)
			, Root(InRoot)
			, Revision(InRevision)
			, DocumentHash(InDocumentHash)
			, Started(FPlatformTime::Seconds())
		{
		}

		virtual bool Update() override
		{
			if (FPlatformTime::Seconds() - Started > 45.0)
			{
				Test->AddError(TEXT("Creator Registry HTTP fixture timed out."));
				Cleanup();
				return true;
			}
			FAkUGCReleaseRegistryPanelTestAccess::Poll(*Panel);
			if (FAkUGCReleaseRegistryPanelTestAccess::Pending(*Panel))
			{
				return false;
			}

			if (Stage == 0)
			{
				if (FAkUGCReleaseRegistryPanelTestAccess::ReleaseStatus(*Panel)
					!= TEXT("submitted"))
				{
					return Fail(TEXT("Registry submit did not return submitted."));
				}
				FAkUGCReleaseRegistryPanelTestAccess::Refresh(*Panel);
				if (!FAkUGCReleaseRegistryPanelTestAccess::Pending(*Panel))
				{
					return Fail(TEXT("Registry status refresh did not start."));
				}
				Stage = 1;
				return false;
			}
			if (Stage == 1)
			{
				if (FAkUGCReleaseRegistryPanelTestAccess::ReleaseStatus(*Panel)
					!= TEXT("approved"))
				{
					return Fail(TEXT("Registry refresh did not return approved."));
				}
				FAkUGCReleaseRegistryPanelTestAccess::RefreshProject(*Panel);
				if (!FAkUGCReleaseRegistryPanelTestAccess::Pending(*Panel))
				{
					return Fail(TEXT("Cloud Project refresh did not start."));
				}
				Stage = 2;
				return false;
			}
			if (Stage == 2)
			{
				if (FAkUGCReleaseRegistryPanelTestAccess::ProjectAccessRole(*Panel)
						!= TEXT("owner")
					|| FAkUGCReleaseRegistryPanelTestAccess::MemberCount(*Panel)
						!= 1
					|| FAkUGCReleaseRegistryPanelTestAccess::VersionCount(*Panel)
						!= 1)
				{
					return Fail(
						TEXT("Cloud Project, member, or version refresh mismatch."));
				}
				FAkUGCReleaseRegistryPanelTestAccess::SetMember(
					*Panel,
					TEXT("editor-b"),
					TEXT("editor"));
				if (!FAkUGCReleaseRegistryPanelTestAccess::Pending(*Panel))
				{
					return Fail(TEXT("Set collaborator did not start."));
				}
				Stage = 3;
				return false;
			}
			if (Stage == 3)
			{
				if (FAkUGCReleaseRegistryPanelTestAccess::MemberCount(*Panel)
					!= 2)
				{
					return Fail(TEXT("Collaborator was not reflected after set."));
				}
				FAkUGCReleaseRegistryPanelTestAccess::RemoveMember(
					*Panel,
					TEXT("editor-b"));
				if (!FAkUGCReleaseRegistryPanelTestAccess::Pending(*Panel))
				{
					return Fail(TEXT("Remove collaborator did not start."));
				}
				Stage = 4;
				return false;
			}
			if (Stage == 4)
			{
				if (FAkUGCReleaseRegistryPanelTestAccess::MemberCount(*Panel)
					!= 1)
				{
					return Fail(
						TEXT("Collaborator was not removed after refresh."));
				}
				FAkUGCReleaseRegistryPanelTestAccess::Publish(*Panel);
				if (!FAkUGCReleaseRegistryPanelTestAccess::Pending(*Panel))
				{
					return Fail(TEXT("Registry publish did not start."));
				}
				Stage = 5;
				return false;
			}
			if (FAkUGCReleaseRegistryPanelTestAccess::ReleaseStatus(*Panel)
				!= TEXT("published"))
			{
				return Fail(TEXT("Registry publish did not return published."));
			}
			Test->TestEqual(
				TEXT("Registry flow preserves Creator revision"),
				Subsystem->GetDocumentRevision(),
				Revision);
			Test->TestEqual(
				TEXT("Registry flow preserves Creator document"),
				FAkUGCLogicPackHasher::HashDocument(Subsystem->GetDocument()),
				DocumentHash);
			UE_LOG(
				LogTemp,
				Display,
				TEXT("[AkUGCReleaseRegistryHTTP] Passed Operations=16 LoopbackOnly=1"));
			Cleanup();
			return true;
		}

	private:
		bool Fail(const TCHAR* Message)
		{
			Test->AddError(
				FString::Printf(
					TEXT("%s Status=%s"),
					Message,
					*FAkUGCReleaseRegistryPanelTestAccess::Status(*Panel)));
			Cleanup();
			return true;
		}

		void Cleanup()
		{
			Panel.Reset();
			if (Subsystem)
			{
				Subsystem->CloseProject();
			}
			IFileManager::Get().DeleteDirectory(*Root, false, true);
		}

		FAutomationTestBase* Test = nullptr;
		UAkUGCEditorSubsystem* Subsystem = nullptr;
		TSharedPtr<SAkUGCWorkLibraryPanel> Panel;
		FString Root;
		uint64 Revision = 0;
		FString DocumentHash;
		double Started = 0.0;
		int32 Stage = 0;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAkUGCReleaseRegistryHttpTest,
	"AkUGC.Editor.ReleaseRegistry.RealHttpProtocol",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCReleaseRegistryHttpTest::RunTest(const FString& Parameters)
{
	FString BaseUrl;
	if (!FParse::Value(
			FCommandLine::Get(),
			TEXT("RegistryFixture="),
			BaseUrl))
	{
		AddWarning(TEXT("Creator Registry HTTP fixture is not configured."));
		return true;
	}
	UAkUGCEditorSubsystem* Subsystem =
		GEditor
			? GEditor->GetEditorSubsystem<UAkUGCEditorSubsystem>()
			: nullptr;
	if (!Subsystem)
	{
		AddError(TEXT("Missing Creator subsystem."));
		return false;
	}
	FString Error;
	if (!Subsystem->NewTowerDefenseProject(&Error))
	{
		AddError(Error);
		return false;
	}
	const uint64 Revision = Subsystem->GetDocumentRevision();
	const FString DocumentHash =
		FAkUGCLogicPackHasher::HashDocument(Subsystem->GetDocument());
	const FString Root = FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("Automation/CreatorRegistry"),
		FGuid::NewGuid().ToString(EGuidFormats::Digits));
	FAkUGCLogicPackKeyPair KeyPair;
	FAkUGCWorkEntry Entry;
	FString SharePath;
	if (!FAkUGCLogicPackSigner::GenerateKeyPair(KeyPair, &Error)
		|| !FAkUGCWorkLibrary(FPaths::Combine(Root, TEXT("Library"))).Publish(
			Subsystem->GetDocument(),
			KeyPair.PrivateKey,
			FPaths::Combine(Root, TEXT("Share")),
			Entry,
			SharePath,
			&Error))
	{
		AddError(Error);
		Subsystem->CloseProject();
		IFileManager::Get().DeleteDirectory(*Root, false, true);
		return false;
	}
	const TSharedRef<SAkUGCWorkLibraryPanel> Panel =
		SNew(SAkUGCWorkLibraryPanel, Subsystem);
	TestTrue(
		TEXT("Cancel action signals the active Registry request"),
		FAkUGCReleaseRegistryPanelTestAccess::CheckCancellation(*Panel));
	FAkUGCReleaseRegistryPanelTestAccess::Configure(
		*Panel,
		BaseUrl,
		TEXT("registry-creator-token"),
		Entry,
		SharePath);
	FAkUGCReleaseRegistryPanelTestAccess::Submit(*Panel);
	if (!FAkUGCReleaseRegistryPanelTestAccess::Pending(*Panel))
	{
		AddError(
			TEXT("Creator Registry submit did not start: ")
			+ FAkUGCReleaseRegistryPanelTestAccess::Status(*Panel));
		Subsystem->CloseProject();
		IFileManager::Get().DeleteDirectory(*Root, false, true);
		return false;
	}
	ADD_LATENT_AUTOMATION_COMMAND(
		AkUGCReleaseRegistryHttpTest::FObserve(
			this,
			Subsystem,
			Panel,
			Root,
			Revision,
			DocumentHash));
	return true;
}
#endif

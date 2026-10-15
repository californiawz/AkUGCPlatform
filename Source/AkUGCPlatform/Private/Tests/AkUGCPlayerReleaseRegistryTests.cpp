#include "Misc/AutomationTest.h"

#include "Containers/StringConv.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Pack/AkUGCLogicPackBuilder.h"
#include "Pack/AkUGCLogicPackCodec.h"
#include "Pack/AkUGCLogicPackHasher.h"
#include "Pack/AkUGCLogicPackSignature.h"
#include "Prefab/AkUGCOfficialPrefabCatalog.h"
#include "Prefab/AkUGCPrefabRegistry.h"
#include "Release/AkUGCPlayerReleaseRegistryClient.h"
#include "Template/AkUGCTowerDefenseTemplate.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AkUGCPlayerRegistryTest
{
	struct FFixture
	{
		FAkUGCLogicPack Pack;
		FAkUGCLogicPackKeyPair KeyPair;
		TArray<uint8> Artifact;
		FString Url;
		FString WorkUrl;
	};

	bool BuildFixture(FFixture& Out, FString& OutError)
	{
		FAkUGCPrefabRegistry Registry;
		FAkUGCProjectDocument Document;
		if (!FAkUGCOfficialPrefabCatalog::RegisterTowerDefense(
				Registry,
				&OutError)
			|| !FAkUGCTowerDefenseTemplate::CreateDefaultDocument(
				Registry,
				Document,
				&OutError))
		{
			return false;
		}
		Document.Manifest.DisplayName =
			TEXT("Player Registry 直接获取夹具");
		FAkUGCLogicPackBuildResult Build =
			FAkUGCLogicPackBuilder::Build(Document);
		if (!Build.bSucceeded)
		{
			OutError = Build.ErrorMessage;
			return false;
		}
		Out.Pack = MoveTemp(Build.Pack);
		if (!FAkUGCLogicPackSigner::GenerateKeyPair(
				Out.KeyPair,
				&OutError)
			|| !FAkUGCLogicPackSigner::Sign(
				Out.Pack.Manifest,
				Out.KeyPair.PrivateKey,
				Out.Pack.Signature,
				&OutError))
		{
			return false;
		}
		FString Json;
		if (!FAkUGCLogicPackCodec::Serialize(
			Out.Pack,
			Json,
			&OutError))
		{
			return false;
		}
		FTCHARToUTF8 Bytes(*Json, Json.Len());
		Out.Artifact.Append(
			reinterpret_cast<const uint8*>(Bytes.Get()),
			Bytes.Length());
		Out.Url =
			TEXT("http://127.0.0.1:18088/v1/releases/")
			+ Out.Pack.Manifest.ReleaseId.ToString(
				EGuidFormats::DigitsWithHyphensLower)
			+ TEXT("/download");
		Out.WorkUrl =
			TEXT("http://127.0.0.1:18088/v1/works/")
			+ Out.Pack.Manifest.ProjectId.ToString(
				EGuidFormats::DigitsWithHyphensLower);
		return true;
	}

	FAkUGCHttpResult Result(
		const FFixture& Fixture,
		const TArray<uint8>& Artifact,
		const FString& ContentHash)
	{
		FAkUGCHttpResult Value;
		Value.StatusCode = 200;
		Value.EffectiveUrl = Fixture.Url;
		Value.Body = Artifact;
		Value.Headers = {
			{TEXT("Content-Type"), TEXT("application/vnd.akugc+json")},
			{TEXT("Content-Length"), FString::FromInt(Artifact.Num())},
			{
				TEXT("ETag"),
				FString::Printf(
					TEXT("\"sha256:%s\""),
					*FAkUGCLogicPackHasher::Sha256Hex(Artifact))
			},
			{TEXT("X-AkUGC-Content-Hash"), ContentHash}
		};
		return Value;
	}

	FString WorkJson(
		const FFixture& Fixture,
		const TCHAR* Visibility = TEXT("public"),
		const TCHAR* Status = TEXT("published"))
	{
		return FString::Printf(
			TEXT("{\"work\":{")
			TEXT("\"projectId\":\"%s\",")
			TEXT("\"displayName\":\"Player Registry 作品页\",")
			TEXT("\"visibility\":\"%s\",")
			TEXT("\"revision\":2,")
			TEXT("\"updatedAt\":\"2026-10-15T06:02:00Z\",")
			TEXT("\"latestRelease\":{")
			TEXT("\"releaseId\":\"%s\",")
			TEXT("\"projectId\":\"%s\",")
			TEXT("\"schemaVersion\":%d,")
			TEXT("\"ownerSubject\":\"creator-a\",")
			TEXT("\"submittedBySubject\":\"creator-a\",")
			TEXT("\"publisherPublicKey\":\"%s\",")
			TEXT("\"contentHash\":\"%s\",")
			TEXT("\"artifactSha256\":\"%s\",")
			TEXT("\"artifactSize\":%d,")
			TEXT("\"status\":\"%s\",")
			TEXT("\"revision\":3,")
			TEXT("\"submittedAt\":\"2026-10-15T06:00:00Z\",")
			TEXT("\"reviewedAt\":\"2026-10-15T06:01:00Z\",")
			TEXT("\"publishedAt\":\"2026-10-15T06:02:00Z\",")
			TEXT("\"updatedAt\":\"2026-10-15T06:02:00Z\"")
			TEXT("}}}"),
			*Fixture.Pack.Manifest.ProjectId.ToString(
				EGuidFormats::DigitsWithHyphensLower),
			Visibility,
			*Fixture.Pack.Manifest.ReleaseId.ToString(
				EGuidFormats::DigitsWithHyphensLower),
			*Fixture.Pack.Manifest.ProjectId.ToString(
				EGuidFormats::DigitsWithHyphensLower),
			Fixture.Pack.Manifest.SchemaVersion,
			*Fixture.KeyPair.PublicKey,
			*Fixture.Pack.Manifest.ContentHash,
			*FAkUGCLogicPackHasher::Sha256Hex(Fixture.Artifact),
			Fixture.Artifact.Num(),
			Status);
	}

	FAkUGCHttpResult WorkResult(
		const FFixture& Fixture,
		const FString& Json)
	{
		FAkUGCHttpResult Value;
		Value.StatusCode = 200;
		Value.EffectiveUrl = Fixture.WorkUrl;
		FTCHARToUTF8 Bytes(*Json);
		Value.Body.Append(
			reinterpret_cast<const uint8*>(Bytes.Get()),
			Bytes.Length());
		Value.Headers = {
			{
				TEXT("Content-Type"),
				TEXT("application/json; charset=utf-8")
			}
		};
		return Value;
	}

	bool Import(
		const FFixture& Fixture,
		const FAkUGCHttpResult& Result,
		const FString& Key,
		const FString& Library,
		FString& OutFailure,
		FString& OutError)
	{
		FAkUGCWorkEntry Entry;
		FString ArtifactSha256;
		return FAkUGCPlayerReleaseRegistryClient::ImportDownloaded(
			Result,
			Fixture.Url,
			Fixture.Pack.Manifest.ReleaseId,
			Key,
			Library,
			Entry,
			ArtifactSha256,
			OutFailure,
			OutError);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAkUGCPlayerRegistryConfigurationTest,
	"AkUGC.Runtime.ReleaseRegistry.ConfigurationBoundaries",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FAkUGCPlayerRegistryConfigurationTest::RunTest(
	const FString& Parameters)
{
	FString Normalized;
	FString Error;
	TestTrue(
		TEXT("IPv4 loopback Registry is accepted"),
		FAkUGCPlayerReleaseRegistryClient::NormalizeBaseUrl(
			TEXT("http://127.0.0.1:8088/"),
			Normalized,
			Error));
	TestEqual(
		TEXT("Trailing slash is normalized"),
		Normalized,
		FString(TEXT("http://127.0.0.1:8088")));
	TestTrue(
		TEXT("localhost Registry is accepted"),
		FAkUGCPlayerReleaseRegistryClient::NormalizeBaseUrl(
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
			*FString::Printf(
				TEXT("Invalid base is rejected: %s"),
				*Invalid),
			FAkUGCPlayerReleaseRegistryClient::NormalizeBaseUrl(
				Invalid,
				Normalized,
				Error));
	}
	TestTrue(
		TEXT("Visible ASCII Player token is accepted"),
		FAkUGCPlayerReleaseRegistryClient::ValidateBearerToken(
			TEXT("player-token_123456789"),
			Error));
	TestFalse(
		TEXT("Header injection token is rejected"),
		FAkUGCPlayerReleaseRegistryClient::ValidateBearerToken(
			TEXT("player\r\nX-Injected: yes"),
			Error));
	TestFalse(
		TEXT("Non-ASCII token is rejected"),
		FAkUGCPlayerReleaseRegistryClient::ValidateBearerToken(
			TEXT("player-令牌"),
			Error));
	TestTrue(
		TEXT("Independent public key is normalized"),
		FAkUGCPlayerReleaseRegistryClient::NormalizeTrustedPublicKey(
			FString::ChrN(64, TEXT('A')),
			Normalized,
			Error));
	TestEqual(
		TEXT("Trusted key canonical form is lowercase"),
		Normalized,
		FString::ChrN(64, TEXT('a')));
	TestFalse(
		TEXT("Malformed trusted key is rejected"),
		FAkUGCPlayerReleaseRegistryClient::NormalizeTrustedPublicKey(
			TEXT("server-says-trusted"),
			Normalized,
			Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAkUGCPlayerRegistryWorkPageTest,
	"AkUGC.Runtime.ReleaseRegistry.WorkPageBoundaries",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FAkUGCPlayerRegistryWorkPageTest::RunTest(
	const FString& Parameters)
{
	using namespace AkUGCPlayerRegistryTest;
	FFixture Fixture;
	FString Error;
	if (!BuildFixture(Fixture, Error))
	{
		AddError(Error);
		return false;
	}
	FAkUGCPlayerRegistryWorkPage Work;
	FString Failure;
	TestTrue(
		TEXT("Published public work page resolves a Release ID"),
		FAkUGCPlayerReleaseRegistryClient::ParseWorkPage(
			WorkResult(Fixture, WorkJson(Fixture)),
			Fixture.WorkUrl,
			Fixture.Pack.Manifest.ProjectId,
			Work,
			Failure,
			Error));
	TestEqual(
		TEXT("Work page remains bound to requested Project"),
		Work.ProjectId,
		Fixture.Pack.Manifest.ProjectId);
	TestEqual(
		TEXT("Work page resolves latest published Release"),
		Work.LatestReleaseId,
		Fixture.Pack.Manifest.ReleaseId);
	TestEqual(
		TEXT("Visibility is preserved for display"),
		Work.Visibility,
		FString(TEXT("public")));

	TestFalse(
		TEXT("Non-published work metadata is rejected"),
		FAkUGCPlayerReleaseRegistryClient::ParseWorkPage(
			WorkResult(
				Fixture,
				WorkJson(Fixture, TEXT("public"), TEXT("revoked"))),
			Fixture.WorkUrl,
			Fixture.Pack.Manifest.ProjectId,
			Work,
			Failure,
			Error));
	TestEqual(
		TEXT("Invalid work state is a protocol failure"),
		Failure,
		FString(TEXT("protocol")));

	FString WrongProject = WorkJson(Fixture);
	WrongProject.ReplaceInline(
		*Fixture.Pack.Manifest.ProjectId.ToString(
			EGuidFormats::DigitsWithHyphensLower),
		TEXT("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"),
		ESearchCase::CaseSensitive);
	TestFalse(
		TEXT("Work page for another Project is rejected"),
		FAkUGCPlayerReleaseRegistryClient::ParseWorkPage(
			WorkResult(Fixture, WrongProject),
			Fixture.WorkUrl,
			Fixture.Pack.Manifest.ProjectId,
			Work,
			Failure,
			Error));

	FString Unknown = WorkJson(Fixture);
	Unknown = TEXT("{\"serverTrusted\":true,") + Unknown.Mid(1);
	TestFalse(
		TEXT("Unknown work response fields are rejected"),
		FAkUGCPlayerReleaseRegistryClient::ParseWorkPage(
			WorkResult(Fixture, Unknown),
			Fixture.WorkUrl,
			Fixture.Pack.Manifest.ProjectId,
			Work,
			Failure,
			Error));

	FAkUGCHttpResult DuplicateContentType =
		WorkResult(Fixture, WorkJson(Fixture));
	DuplicateContentType.Headers.Add(
		{
			TEXT("content-type"),
			TEXT("application/json; charset=utf-8")
		});
	TestFalse(
		TEXT("Duplicate work Content-Type is rejected"),
		FAkUGCPlayerReleaseRegistryClient::ParseWorkPage(
			DuplicateContentType,
			Fixture.WorkUrl,
			Fixture.Pack.Manifest.ProjectId,
			Work,
			Failure,
			Error));

	FAkUGCHttpResult NotFound;
	NotFound.StatusCode = 404;
	NotFound.EffectiveUrl = Fixture.WorkUrl;
	TestFalse(
		TEXT("Hidden work page remains unavailable"),
		FAkUGCPlayerReleaseRegistryClient::ParseWorkPage(
			NotFound,
			Fixture.WorkUrl,
			Fixture.Pack.Manifest.ProjectId,
			Work,
			Failure,
			Error));
	TestEqual(
		TEXT("Hidden work uses the not-available verdict"),
		Failure,
		FString(TEXT("not_available")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAkUGCPlayerRegistryArtifactTest,
	"AkUGC.Runtime.ReleaseRegistry.LocalTrustAndArtifactBoundaries",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FAkUGCPlayerRegistryArtifactTest::RunTest(
	const FString& Parameters)
{
	using namespace AkUGCPlayerRegistryTest;
	FFixture Fixture;
	FString Error;
	if (!BuildFixture(Fixture, Error))
	{
		AddError(Error);
		return false;
	}
	const FString Root = FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("Automation/PlayerRegistry"),
		FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FAkUGCHttpResult Good = Result(
		Fixture,
		Fixture.Artifact,
		Fixture.Pack.Manifest.ContentHash);
	FString Failure;
	TestTrue(
		TEXT("Published response imports after local verification"),
		Import(
			Fixture,
			Good,
			Fixture.KeyPair.PublicKey,
			FPaths::Combine(Root, TEXT("Good")),
			Failure,
			Error));
	TestTrue(
		TEXT("Imported response remains locally verifiable"),
		FAkUGCWorkLibrary(FPaths::Combine(Root, TEXT("Good")))
			.Load(Fixture.Pack.Manifest.ReleaseId)
			.bSucceeded);

	FAkUGCLogicPackKeyPair WrongKey;
	FAkUGCLogicPackSigner::GenerateKeyPair(WrongKey, &Error);
	TestFalse(
		TEXT("Registry response cannot replace independent trust"),
		Import(
			Fixture,
			Good,
			WrongKey.PublicKey,
			FPaths::Combine(Root, TEXT("WrongKey")),
			Failure,
			Error));
	TestEqual(
		TEXT("Wrong trust root is a local verification failure"),
		Failure,
		FString(TEXT("local_verification")));

	FAkUGCLogicPack TamperedPack = Fixture.Pack;
	TamperedPack.Document.Manifest.DisplayName =
		TEXT("服务器声称可信的篡改内容");
	FString TamperedJson;
	FAkUGCLogicPackCodec::Serialize(TamperedPack, TamperedJson);
	FTCHARToUTF8 TamperedUtf8(*TamperedJson, TamperedJson.Len());
	TArray<uint8> TamperedBytes;
	TamperedBytes.Append(
		reinterpret_cast<const uint8*>(TamperedUtf8.Get()),
		TamperedUtf8.Length());
	const FAkUGCHttpResult Tampered = Result(
		Fixture,
		TamperedBytes,
		TamperedPack.Manifest.ContentHash);
	TestFalse(
		TEXT("Matching transport digest cannot authorize tampered content"),
		Import(
			Fixture,
			Tampered,
			Fixture.KeyPair.PublicKey,
			FPaths::Combine(Root, TEXT("Tampered")),
			Failure,
			Error));
	TestEqual(
		TEXT("Tampered download fails local verification"),
		Failure,
		FString(TEXT("local_verification")));

	FAkUGCLogicPack OtherRelease = Fixture.Pack;
	const FGuid OtherReleaseId = FGuid::NewGuid();
	OtherRelease.Manifest.ReleaseId = OtherReleaseId;
	FAkUGCLogicPackSigner::Sign(
		OtherRelease.Manifest,
		Fixture.KeyPair.PrivateKey,
		OtherRelease.Signature,
		&Error);
	FString OtherJson;
	FAkUGCLogicPackCodec::Serialize(OtherRelease, OtherJson);
	FTCHARToUTF8 OtherUtf8(*OtherJson, OtherJson.Len());
	TArray<uint8> OtherBytes;
	OtherBytes.Append(
		reinterpret_cast<const uint8*>(OtherUtf8.Get()),
		OtherUtf8.Length());
	const FString MismatchLibrary =
		FPaths::Combine(Root, TEXT("ReleaseMismatch"));
	TestFalse(
		TEXT("Valid signed response cannot substitute another Release ID"),
		Import(
			Fixture,
			Result(
				Fixture,
				OtherBytes,
				OtherRelease.Manifest.ContentHash),
			Fixture.KeyPair.PublicKey,
			MismatchLibrary,
			Failure,
			Error));
	TestFalse(
		TEXT("Release mismatch is rejected before library side effects"),
		FAkUGCWorkLibrary(MismatchLibrary)
			.Load(OtherReleaseId)
			.bSucceeded);

	FAkUGCHttpResult WrongDigest = Good;
	for (FAkUGCHttpHeader& Header : WrongDigest.Headers)
	{
		if (Header.Name == TEXT("ETag"))
		{
			Header.Value =
				TEXT("\"sha256:")
				+ FString::ChrN(64, TEXT('0'))
				+ TEXT("\"");
		}
	}
	TestFalse(
		TEXT("Artifact ETag mismatch is rejected"),
		Import(
			Fixture,
			WrongDigest,
			Fixture.KeyPair.PublicKey,
			FPaths::Combine(Root, TEXT("WrongDigest")),
			Failure,
			Error));
	TestEqual(
		TEXT("Digest mismatch has an explicit verdict"),
		Failure,
		FString(TEXT("artifact_digest")));

	FAkUGCHttpResult DuplicateHeader = Good;
	DuplicateHeader.Headers.Add(
		{TEXT("etag"), DuplicateHeader.Headers[2].Value});
	TestFalse(
		TEXT("Duplicate security header is rejected"),
		Import(
			Fixture,
			DuplicateHeader,
			Fixture.KeyPair.PublicKey,
			FPaths::Combine(Root, TEXT("Duplicate")),
			Failure,
			Error));
	TestEqual(
		TEXT("Duplicate header is a protocol failure"),
		Failure,
		FString(TEXT("protocol")));

	FAkUGCHttpResult Encoded = Good;
	Encoded.Headers.Add({TEXT("Content-Encoding"), TEXT("gzip")});
	TestFalse(
		TEXT("Encoded artifact response is rejected"),
		Import(
			Fixture,
			Encoded,
			Fixture.KeyPair.PublicKey,
			FPaths::Combine(Root, TEXT("Encoded")),
			Failure,
			Error));

	FAkUGCHttpResult TooLarge;
	TooLarge.bTooLarge = true;
	TestFalse(
		TEXT("Receive-time body overflow is rejected"),
		Import(
			Fixture,
			TooLarge,
			Fixture.KeyPair.PublicKey,
			FPaths::Combine(Root, TEXT("TooLarge")),
			Failure,
			Error));
	TestEqual(
		TEXT("Overflow has an explicit verdict"),
		Failure,
		FString(TEXT("response_too_large")));

	FAkUGCHttpResult NotFound;
	NotFound.StatusCode = 404;
	NotFound.EffectiveUrl = Fixture.Url;
	TestFalse(
		TEXT("Unavailable release is not imported"),
		Import(
			Fixture,
			NotFound,
			Fixture.KeyPair.PublicKey,
			FPaths::Combine(Root, TEXT("NotFound")),
			Failure,
			Error));
	TestEqual(
		TEXT("Hidden release uses the not-available verdict"),
		Failure,
		FString(TEXT("not_available")));

	FAkUGCHttpResult Redirect;
	Redirect.StatusCode = 302;
	Redirect.EffectiveUrl = Fixture.Url;
	TestFalse(
		TEXT("HTTP redirect is not followed as a trusted artifact"),
		Import(
			Fixture,
			Redirect,
			Fixture.KeyPair.PublicKey,
			FPaths::Combine(Root, TEXT("Redirect")),
			Failure,
			Error));
	TestEqual(
		TEXT("Redirect has an explicit verdict"),
		Failure,
		FString(TEXT("redirect")));

	IFileManager::Get().DeleteDirectory(*Root, false, true);
	return true;
}
#endif

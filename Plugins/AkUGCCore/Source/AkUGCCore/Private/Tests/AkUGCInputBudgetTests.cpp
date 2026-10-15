#include "Misc/AutomationTest.h"

#include "Command/AkUGCCommand.h"
#include "Command/AkUGCCommandJson.h"
#include "Document/AkUGCDocument.h"
#include "Document/AkUGCDocumentJson.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Validation/AkUGCDocumentValidator.h"
#include "Validation/AkUGCInputBudget.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAkUGCInputBudgetTest,
	"AkUGC.Core.InputBudget.RejectsOversizedAndAmplifiedInputs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCInputBudgetTest::RunTest(const FString& Parameters)
{
	FString Error;

	FString Multibyte;
	Multibyte.Reserve(
		static_cast<int32>(AkUGCInputBudget::MaxCopilotRequestBytes / 2));
	for (int32 Index = 0;
		Index < AkUGCInputBudget::MaxCopilotRequestBytes / 2;
		++Index)
	{
		Multibyte.AppendChar(TEXT('界'));
	}
	TestFalse(
		TEXT("UTF-8 bytes cannot bypass a character-count budget"),
		AkUGCInputBudget::ValidateUtf8Text(
			Multibyte,
			AkUGCInputBudget::MaxCopilotRequestBytes,
			TEXT("Multibyte fixture"),
			&Error));
	TestTrue(TEXT("UTF-8 rejection reports the byte budget"), Error.Contains(TEXT("UTF-8 bytes")));

	FString DeepJson;
	for (int32 Depth = 0; Depth <= AkUGCInputBudget::MaxJsonDepth; ++Depth)
	{
		DeepJson += TEXT("[");
	}
	DeepJson += TEXT("0");
	for (int32 Depth = 0; Depth <= AkUGCInputBudget::MaxJsonDepth; ++Depth)
	{
		DeepJson += TEXT("]");
	}
	TestFalse(
		TEXT("JSON nesting beyond the parser budget is rejected"),
		AkUGCInputBudget::ValidateJson(
			DeepJson,
			AkUGCInputBudget::MaxProjectDocumentBytes,
			TEXT("Deep fixture"),
			&Error));
	TestTrue(TEXT("Depth rejection is explicit"), Error.Contains(TEXT("nesting budget")));

	FString TokenHeavyJson;
	TokenHeavyJson.Reserve(AkUGCInputBudget::MaxJsonTokens * 2 + 2);
	TokenHeavyJson += TEXT("[");
	for (int32 Index = 0; Index < AkUGCInputBudget::MaxJsonTokens; ++Index)
	{
		if (Index > 0)
		{
			TokenHeavyJson += TEXT(",");
		}
		TokenHeavyJson += TEXT("0");
	}
	TokenHeavyJson += TEXT("]");
	TestFalse(
		TEXT("JSON token amplification is rejected before DOM parsing"),
		AkUGCInputBudget::ValidateJson(
			TokenHeavyJson,
			AkUGCInputBudget::MaxProjectDocumentBytes,
			TEXT("Token fixture"),
			&Error));
	TestTrue(TEXT("Token rejection is explicit"), Error.Contains(TEXT("token budget")));

	TestFalse(
		TEXT("Duplicate fields remain rejected by the shared guard"),
		AkUGCInputBudget::ValidateJson(
			TEXT("{\"value\":1,\"value\":2}"),
			1024,
			TEXT("Duplicate fixture"),
			&Error));
	TestTrue(TEXT("Duplicate rejection is explicit"), Error.Contains(TEXT("duplicated")));
	TestFalse(
		TEXT("Escaped duplicate fields are rejected after identifier decoding"),
		AkUGCInputBudget::ValidateJson(
			TEXT("{\"name\":1,\"\\u006eame\":2}"),
			1024,
			TEXT("Escaped duplicate fixture"),
			&Error));
	TestTrue(TEXT("Escaped duplicate rejection is explicit"),
		Error.Contains(TEXT("duplicated")));

	FString CommandsJson = TEXT("{\"commands\":[");
	for (int32 Index = 0;
		Index <= AkUGCInputBudget::MaxCommandsPerTransaction;
		++Index)
	{
		if (Index > 0)
		{
			CommandsJson += TEXT(",");
		}
		CommandsJson += TEXT("{}");
	}
	CommandsJson += TEXT("]}");
	FAkUGCCommandTransaction Transaction;
	TestFalse(
		TEXT("Command arrays beyond the transaction budget are rejected"),
		FAkUGCCommandJson::Deserialize(CommandsJson, Transaction, &Error));
	TestTrue(TEXT("Command-count rejection is explicit"), Error.Contains(TEXT("command budget")));

	FAkUGCProjectDocument OversizedDocument;
	OversizedDocument.Manifest.SchemaVersion = AkUGCSchema::CurrentProjectDocumentVersion;
	OversizedDocument.Manifest.ProjectId = FGuid::NewGuid();
	OversizedDocument.Manifest.DisplayName = TEXT("Input budget fixture");
	OversizedDocument.Manifest.TemplateId = TEXT("official.tower_defense");
	FAkUGCSceneDocument& Scene = OversizedDocument.Scenes.AddDefaulted_GetRef();
	Scene.SceneId = FGuid::NewGuid();
	Scene.Entities.SetNum(AkUGCDocumentLimits::MaxEntities + 1);
	const FAkUGCValidationResult Validation =
		FAkUGCDocumentValidator::Validate(OversizedDocument);
	TestFalse(TEXT("Document entity amplification is rejected"), Validation.IsValid());
	TestTrue(
		TEXT("Document entity rejection is explicit"),
		Validation.Issues.ContainsByPredicate([](const FAkUGCValidationIssue& Issue)
		{
			return Issue.Message.Contains(TEXT("entity input budget"));
		}));
	FString OversizedDocumentJson;
	TestTrue(
		TEXT("Oversized in-memory fixture can be serialized for negative testing"),
		FAkUGCDocumentJson::Serialize(
			OversizedDocument,
			OversizedDocumentJson,
			&Error));
	FAkUGCProjectDocument RejectedDocument;
	TestFalse(
		TEXT("Document deserialization enforces structural input budgets"),
		FAkUGCDocumentJson::Deserialize(
			OversizedDocumentJson,
			RejectedDocument,
			&Error));
	TestTrue(
		TEXT("Deserialization structural rejection is explicit"),
		Error.Contains(TEXT("entity input budget")));

	FAkUGCProjectDocument OversizedStringDocument;
	OversizedStringDocument.Manifest.SchemaVersion =
		AkUGCSchema::CurrentProjectDocumentVersion;
	FAkUGCSceneDocument& StringScene =
		OversizedStringDocument.Scenes.AddDefaulted_GetRef();
	FAkUGCEntityRecord& StringEntity =
		StringScene.Entities.AddDefaulted_GetRef();
	FAkUGCComponentRecord& StringComponent =
		StringEntity.Components.AddDefaulted_GetRef();
	FAkUGCValue& StringValue =
		StringComponent.Properties.Add(TEXT("fixture"));
	StringValue.Type = EAkUGCValueType::String;
	StringValue.StringValue = FString::ChrN(
		AkUGCDocumentLimits::MaxValueStringLength + 1,
		TEXT('x'));
	FString OversizedStringJson;
	TestTrue(
		TEXT("Oversized string fixture can be serialized for negative testing"),
		FAkUGCDocumentJson::Serialize(
			OversizedStringDocument,
			OversizedStringJson,
			&Error));
	TestFalse(
		TEXT("Document deserialization enforces nested string budgets"),
		FAkUGCDocumentJson::Deserialize(
			OversizedStringJson,
			RejectedDocument,
			&Error));
	TestTrue(
		TEXT("Nested string rejection is explicit"),
		Error.Contains(TEXT("value text exceeds")));

	const FString OversizedPath = FPaths::CreateTempFilename(
		*FPaths::ProjectSavedDir(),
		TEXT("AkUGCInputBudget"),
		TEXT(".json"));
	TArray<uint8> OversizedBytes;
	OversizedBytes.SetNumZeroed(
		static_cast<int32>(AkUGCInputBudget::MaxProjectDocumentBytes + 1));
	TestTrue(
		TEXT("Oversized file fixture writes"),
		FFileHelper::SaveArrayToFile(OversizedBytes, *OversizedPath));
	FString Loaded;
	TestFalse(
		TEXT("File size is rejected before allocation and decoding"),
		AkUGCInputBudget::LoadTextFile(
			OversizedPath,
			AkUGCInputBudget::MaxProjectDocumentBytes,
			TEXT("Oversized project fixture"),
			Loaded,
			&Error));
	TestTrue(TEXT("File-size rejection is explicit"), Error.Contains(TEXT("byte input budget")));
	IFileManager::Get().Delete(*OversizedPath, false, true, true);

	return true;
}

#endif

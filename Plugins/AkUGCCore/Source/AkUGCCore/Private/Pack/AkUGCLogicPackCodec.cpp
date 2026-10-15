#include "Pack/AkUGCLogicPackCodec.h"

#include "Document/AkUGCDocumentJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Guid.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Validation/AkUGCInputBudget.h"

namespace
{
	constexpr int32 MaxManifestCapabilities = 16;
	constexpr int32 MaxAssetDependencies = 1024;
	constexpr int32 MaxCapabilityCharacters = 64;
	constexpr int32 MaxAssetDependencyCharacters = 256;
	constexpr int32 MaxNameCharacters = 128;

	TSharedPtr<FJsonObject> SerializeManifest(const FAkUGCLogicPackManifest& Manifest)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("releaseId"), Manifest.ReleaseId.ToString(EGuidFormats::DigitsWithHyphensLower));
		Obj->SetNumberField(TEXT("schemaVersion"), Manifest.SchemaVersion);
		Obj->SetStringField(TEXT("projectId"), Manifest.ProjectId.ToString(EGuidFormats::DigitsWithHyphensLower));
		Obj->SetStringField(TEXT("templateId"), Manifest.TemplateId.ToString());

		TArray<TSharedPtr<FJsonValue>> Capabilities;
		for (const FName& Capability : Manifest.Capabilities)
		{
			Capabilities.Add(MakeShared<FJsonValueString>(Capability.ToString()));
		}
		Obj->SetArrayField(TEXT("capabilities"), Capabilities);

		TArray<TSharedPtr<FJsonValue>> Dependencies;
		for (const FString& Dependency : Manifest.AssetDependencies)
		{
			Dependencies.Add(MakeShared<FJsonValueString>(Dependency));
		}
		Obj->SetArrayField(TEXT("assetDependencies"), Dependencies);

		Obj->SetStringField(TEXT("contentHash"), Manifest.ContentHash);
		return Obj;
	}

	TSharedPtr<FJsonObject> SerializeProgram(const FAkUGCLogicProgram& Program)
	{
		const UEnum* OpcodeEnum = StaticEnum<EAkUGCLogicOpcode>();

		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Instructions;
		for (const FAkUGCLogicInstruction& Instruction : Program.Instructions)
		{
			TSharedPtr<FJsonObject> I = MakeShared<FJsonObject>();
			I->SetStringField(TEXT("opcode"), OpcodeEnum->GetNameStringByValue(static_cast<int64>(Instruction.Opcode)));
			I->SetStringField(TEXT("sourceNodeId"), Instruction.SourceNodeId.ToString(EGuidFormats::DigitsWithHyphensLower));
			I->SetStringField(TEXT("operand"), Instruction.Operand);
			I->SetNumberField(TEXT("delaySeconds"), Instruction.DelaySeconds);
			I->SetStringField(TEXT("spawnPrefabId"), Instruction.SpawnPrefabId.ToString());
			I->SetStringField(TEXT("spawnAtEntityId"), Instruction.SpawnAtEntityId.ToString(EGuidFormats::DigitsWithHyphensLower));

			TArray<TSharedPtr<FJsonValue>> Successors;
			for (const int32 Index : Instruction.SuccessorIndices)
			{
				Successors.Add(MakeShared<FJsonValueNumber>(Index));
			}
			I->SetArrayField(TEXT("successorIndices"), Successors);
			Instructions.Add(MakeShared<FJsonValueObject>(I));
		}
		Obj->SetArrayField(TEXT("instructions"), Instructions);
		Obj->SetNumberField(TEXT("gameStartEntryIndex"), Program.GameStartEntryIndex);
		Obj->SetNumberField(TEXT("waveStartEntryIndex"), Program.WaveStartEntryIndex);
		return Obj;
	}

	bool ReadStringArrayField(
		const TSharedPtr<FJsonObject>& Obj,
		const TCHAR* Field,
		int32 MaxCount,
		int32 MaxCharacters,
		TArray<FString>& Out,
		FString& Error)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Obj->TryGetArrayField(Field, Values)
			|| !Values
			|| Values->Num() > MaxCount)
		{
			Error = FString::Printf(
				TEXT("manifest.%s: 字段必须为不超过 %d 项的数组。"),
				Field,
				MaxCount);
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			if (!Value.IsValid()
				|| Value->Type != EJson::String
				|| Value->AsString().Len() > MaxCharacters)
			{
				Error = FString::Printf(
					TEXT("manifest.%s: 数组元素必须为不超过 %d 字符的字符串。"),
					Field,
					MaxCharacters);
				return false;
			}
			Out.Add(Value->AsString());
		}
		return true;
	}

	bool DeserializeManifest(
		const TSharedPtr<FJsonObject>& Obj,
		FAkUGCLogicPackManifest& Out,
		FString& Error)
	{
		if (!Obj.IsValid() || Obj->Values.Num() != 7)
		{
			Error = TEXT("manifest: 字段数量或对象格式错误。");
			return false;
		}
		FString ReleaseIdStr;
		FString ProjectIdStr;
		FString TemplateIdStr;
		FString ContentHash;
		double SchemaVersion = 0.0;

		if (!Obj->TryGetStringField(TEXT("releaseId"), ReleaseIdStr)
			|| !Obj->TryGetNumberField(TEXT("schemaVersion"), SchemaVersion)
			|| !Obj->TryGetStringField(TEXT("projectId"), ProjectIdStr)
			|| !Obj->TryGetStringField(TEXT("templateId"), TemplateIdStr)
			|| !Obj->TryGetStringField(TEXT("contentHash"), ContentHash))
		{
			Error = TEXT("manifest: 必需字段缺失或类型错误。");
			return false;
		}

		if (!FGuid::Parse(ReleaseIdStr, Out.ReleaseId))
		{
			Error = TEXT("manifest.releaseId: GUID 格式非法。");
			return false;
		}
		if (!FGuid::Parse(ProjectIdStr, Out.ProjectId))
		{
			Error = TEXT("manifest.projectId: GUID 格式非法。");
			return false;
		}

		if (!FMath::IsFinite(SchemaVersion)
			|| SchemaVersion != FMath::TruncToDouble(SchemaVersion)
			|| SchemaVersion < 0.0
			|| SchemaVersion > static_cast<double>(MAX_int32))
		{
			Error = TEXT("manifest.schemaVersion: 必须为 int32 范围内的非负整数。");
			return false;
		}
		if (TemplateIdStr.IsEmpty()
			|| TemplateIdStr.Len() > MaxNameCharacters
			|| ContentHash.Len() != 64)
		{
			Error = TEXT("manifest: templateId 或 contentHash 长度非法。");
			return false;
		}
		Out.SchemaVersion = static_cast<int32>(SchemaVersion);
		Out.TemplateId = FName(*TemplateIdStr);
		Out.ContentHash = ContentHash;

		const TArray<TSharedPtr<FJsonValue>>* CapabilityValues = nullptr;
		if (!Obj->TryGetArrayField(TEXT("capabilities"), CapabilityValues)
			|| !CapabilityValues
			|| CapabilityValues->Num() > MaxManifestCapabilities)
		{
			Error = FString::Printf(
				TEXT("manifest.capabilities: 字段必须为不超过 %d 项的数组。"),
				MaxManifestCapabilities);
			return false;
		}
		for (const TSharedPtr<FJsonValue>& CapabilityValue : *CapabilityValues)
		{
			if (!CapabilityValue.IsValid()
				|| CapabilityValue->Type != EJson::String
				|| CapabilityValue->AsString().IsEmpty()
				|| CapabilityValue->AsString().Len() > MaxCapabilityCharacters)
			{
				Error = FString::Printf(
					TEXT("manifest.capabilities: 数组元素必须为 1 至 %d 字符的字符串。"),
					MaxCapabilityCharacters);
				return false;
			}
			Out.Capabilities.Add(FName(*CapabilityValue->AsString()));
		}

		if (!ReadStringArrayField(
				Obj,
				TEXT("assetDependencies"),
				MaxAssetDependencies,
				MaxAssetDependencyCharacters,
				Out.AssetDependencies,
				Error))
		{
			return false;
		}
		return true;
	}

	bool DeserializeProgram(
		const TSharedPtr<FJsonObject>& Obj,
		FAkUGCLogicProgram& Out,
		FString& Error)
	{
		if (!Obj.IsValid() || Obj->Values.Num() != 3)
		{
			Error = TEXT("program: 字段数量或对象格式错误。");
			return false;
		}
		const UEnum* OpcodeEnum = StaticEnum<EAkUGCLogicOpcode>();

		const TArray<TSharedPtr<FJsonValue>>* Instructions = nullptr;
		if (!Obj->TryGetArrayField(TEXT("instructions"), Instructions)
			|| !Instructions
			|| Instructions->Num() > AkUGCLogicLimits::MaxNodes)
		{
			Error = FString::Printf(
				TEXT("program.instructions: 字段必须为不超过 %d 项的数组。"),
				AkUGCLogicLimits::MaxNodes);
			return false;
		}

		int32 TotalSuccessorCount = 0;
		for (int32 InstructionIndex = 0; InstructionIndex < Instructions->Num(); ++InstructionIndex)
		{
			const TSharedPtr<FJsonValue>& Value = (*Instructions)[InstructionIndex];
			if (!Value.IsValid()
				|| Value->Type != EJson::Object
				|| !Value->AsObject().IsValid()
				|| Value->AsObject()->Values.Num() != 7)
			{
				Error = FString::Printf(
					TEXT("program.instructions[%d]: 必须为包含固定字段的对象。"),
					InstructionIndex);
				return false;
			}

			const TSharedPtr<FJsonObject> I = Value->AsObject();
			FString OpcodeName;
			FString SourceNodeIdStr;
			FString Operand;
			double DelaySeconds = 0.0;
			FString SpawnPrefabIdStr;
			FString SpawnAtEntityIdStr;

			if (!I->TryGetStringField(TEXT("opcode"), OpcodeName)
				|| !I->TryGetStringField(TEXT("sourceNodeId"), SourceNodeIdStr)
				|| !I->TryGetStringField(TEXT("operand"), Operand)
				|| !I->TryGetNumberField(TEXT("delaySeconds"), DelaySeconds)
				|| !I->TryGetStringField(TEXT("spawnPrefabId"), SpawnPrefabIdStr)
				|| !I->TryGetStringField(TEXT("spawnAtEntityId"), SpawnAtEntityIdStr))
			{
				Error = FString::Printf(TEXT("program.instructions[%d]: 必需字段缺失或类型错误。"), InstructionIndex);
				return false;
			}
			if (OpcodeName.IsEmpty()
				|| OpcodeName.Len() > MaxNameCharacters
				|| Operand.Len() > AkUGCLogicLimits::MaxMessageLength
				|| SpawnPrefabIdStr.Len() > MaxNameCharacters)
			{
				Error = FString::Printf(
					TEXT("program.instructions[%d]: 操作码、操作数或 Prefab 名称超过输入预算。"),
					InstructionIndex);
				return false;
			}

			FAkUGCLogicInstruction& Instruction = Out.Instructions.AddDefaulted_GetRef();
			const int64 OpcodeValue = OpcodeEnum->GetValueByNameString(*OpcodeName);
			if (OpcodeValue == INDEX_NONE)
			{
				Error = FString::Printf(
					TEXT("program.instructions[%d].opcode: 未知操作码 '%s'。"),
					InstructionIndex,
					*OpcodeName);
				return false;
			}
			Instruction.Opcode = static_cast<EAkUGCLogicOpcode>(OpcodeValue);

			if (!FGuid::Parse(SourceNodeIdStr, Instruction.SourceNodeId))
			{
				Error = FString::Printf(TEXT("program.instructions[%d].sourceNodeId: GUID 格式非法。"), InstructionIndex);
				return false;
			}
			if (!FGuid::Parse(SpawnAtEntityIdStr, Instruction.SpawnAtEntityId))
			{
				Error = FString::Printf(TEXT("program.instructions[%d].spawnAtEntityId: GUID 格式非法。"), InstructionIndex);
				return false;
			}
			if (!FMath::IsFinite(DelaySeconds))
			{
				Error = FString::Printf(TEXT("program.instructions[%d].delaySeconds: 必须为有限数值。"), InstructionIndex);
				return false;
			}

			Instruction.Operand = Operand;
			Instruction.DelaySeconds = DelaySeconds;
			Instruction.SpawnPrefabId = FName(*SpawnPrefabIdStr);

			const TArray<TSharedPtr<FJsonValue>>* Successors = nullptr;
			if (!I->TryGetArrayField(TEXT("successorIndices"), Successors)
				|| !Successors
				|| Successors->Num()
					> AkUGCLogicLimits::MaxConnections - TotalSuccessorCount)
			{
				Error = FString::Printf(
					TEXT("program.instructions[%d].successorIndices: 数组使总边数超过 %d。"),
					InstructionIndex,
					AkUGCLogicLimits::MaxConnections);
				return false;
			}
			TotalSuccessorCount += Successors->Num();
			for (const TSharedPtr<FJsonValue>& Successor : *Successors)
			{
				if (!Successor.IsValid() || Successor->Type != EJson::Number)
				{
					Error = FString::Printf(TEXT("program.instructions[%d].successorIndices: 元素必须为整数。"), InstructionIndex);
					return false;
				}
				const double Number = Successor->AsNumber();
				if (!FMath::IsFinite(Number)
					|| Number != FMath::TruncToDouble(Number)
					|| Number < 0.0
					|| Number >= static_cast<double>(Instructions->Num()))
				{
					Error = FString::Printf(
						TEXT("program.instructions[%d].successorIndices: 元素必须为有效指令索引。"),
						InstructionIndex);
					return false;
				}
				Instruction.SuccessorIndices.Add(static_cast<int32>(Number));
			}
		}

		double GameStartEntryIndex = 0.0;
		double WaveStartEntryIndex = 0.0;
		if (!Obj->TryGetNumberField(TEXT("gameStartEntryIndex"), GameStartEntryIndex)
			|| !Obj->TryGetNumberField(TEXT("waveStartEntryIndex"), WaveStartEntryIndex))
		{
			Error = TEXT("program: 入口索引字段缺失或类型错误。");
			return false;
		}
		const auto IsEntryIndex = [Instructions](double Value)
		{
			return FMath::IsFinite(Value)
				&& Value == FMath::TruncToDouble(Value)
				&& Value >= static_cast<double>(INDEX_NONE)
				&& Value < static_cast<double>(Instructions->Num());
		};
		if (!IsEntryIndex(GameStartEntryIndex)
			|| !IsEntryIndex(WaveStartEntryIndex))
		{
			Error = TEXT("program: 入口索引必须为 -1 或有效指令索引。");
			return false;
		}
		Out.GameStartEntryIndex = static_cast<int32>(GameStartEntryIndex);
		Out.WaveStartEntryIndex = static_cast<int32>(WaveStartEntryIndex);
		return true;
		}

		TSharedPtr<FJsonObject> SerializeSignature(const FAkUGCLogicPackSignature& Signature)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("algorithm"), Signature.Algorithm);
			Obj->SetStringField(TEXT("publicKey"), Signature.PublicKey);
			Obj->SetStringField(TEXT("signature"), Signature.Signature);
			return Obj;
		}

		bool DeserializeSignature(
			const TSharedPtr<FJsonObject>& Obj,
			FAkUGCLogicPackSignature& Out,
			FString& Error)
		{
			if (!Obj.IsValid() || Obj->Values.Num() != 3)
			{
				Error = TEXT("signature: 字段数量或对象格式错误。");
				return false;
			}
			FString Algorithm;
			FString PublicKey;
			FString SignatureValue;
			if (!Obj->TryGetStringField(TEXT("algorithm"), Algorithm)
				|| !Obj->TryGetStringField(TEXT("publicKey"), PublicKey)
				|| !Obj->TryGetStringField(TEXT("signature"), SignatureValue))
			{
				Error = TEXT("signature: 必需字段缺失或类型错误。");
				return false;
			}
			if (Algorithm.IsEmpty()
				|| Algorithm.Len() > 32
				|| PublicKey.Len() > 64
				|| SignatureValue.Len() > 128)
			{
				Error = TEXT("signature: 字段超过签名输入预算。");
				return false;
			}
			Out.Algorithm = Algorithm;
			Out.PublicKey = PublicKey;
			Out.Signature = SignatureValue;
			return true;
		}
		}

bool FAkUGCLogicPackCodec::Serialize(const FAkUGCLogicPack& Pack, FString& OutJson, FString* OutError)
{
	OutJson.Reset();

	// Document 复用既有 JSON 序列化（含校验）。
	FString DocumentJson;
	FString DocumentError;
	if (!FAkUGCDocumentJson::Serialize(Pack.Document, DocumentJson, &DocumentError))
	{
		if (OutError)
		{
			*OutError = FString::Printf(TEXT("document: %s"), *DocumentError);
		}
		return false;
	}

	TSharedPtr<FJsonObject> DocumentObject;
	const TSharedRef<TJsonReader<>> DocumentReader = TJsonReaderFactory<>::Create(DocumentJson);
	if (!FJsonSerializer::Deserialize(DocumentReader, DocumentObject) || !DocumentObject.IsValid())
	{
		if (OutError)
		{
			*OutError = TEXT("document: 序列化结果无法解析回 JSON 对象。");
		}
		return false;
	}

	TSharedPtr<FJsonObject> RootObject = MakeShared<FJsonObject>();
	RootObject->SetObjectField(TEXT("manifest"), SerializeManifest(Pack.Manifest));
	RootObject->SetObjectField(TEXT("document"), DocumentObject);
	RootObject->SetObjectField(TEXT("program"), SerializeProgram(Pack.Program));
	if (!Pack.Signature.Signature.IsEmpty())
	{
		RootObject->SetObjectField(TEXT("signature"), SerializeSignature(Pack.Signature));
	}

	const bool bSucceeded = FJsonSerializer::Serialize(RootObject.ToSharedRef(), TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&OutJson, 0));
	if (!bSucceeded)
	{
		if (OutError)
		{
			*OutError = TEXT("Failed to serialize Logic Pack.");
		}
		return false;
	}
	if (!AkUGCInputBudget::ValidateUtf8Text(
			OutJson,
			AkUGCInputBudget::MaxLogicPackBytes,
			TEXT("Logic Pack"),
			OutError))
	{
		OutJson.Reset();
		return false;
	}
	return true;
}

bool FAkUGCLogicPackCodec::Deserialize(const FString& Json, FAkUGCLogicPack& OutPack, FString* OutError)
{
	OutPack = FAkUGCLogicPack{};
	if (!AkUGCInputBudget::ValidateJson(
			Json,
			AkUGCInputBudget::MaxLogicPackBytes,
			TEXT("Logic Pack"),
			OutError))
	{
		return false;
	}

	TSharedPtr<FJsonObject> RootObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
	{
		if (OutError)
		{
			*OutError = TEXT("Failed to parse Logic Pack JSON.");
		}
		return false;
	}
	const bool bHasSignature = RootObject->HasField(TEXT("signature"));
	if (RootObject->Values.Num() != (bHasSignature ? 4 : 3))
	{
		if (OutError)
		{
			*OutError = TEXT("Logic Pack 只能包含 manifest、document、program 和可选 signature 字段。");
		}
		return false;
	}

	const TSharedPtr<FJsonObject>* ManifestObject = nullptr;
	const TSharedPtr<FJsonObject>* DocumentObject = nullptr;
	const TSharedPtr<FJsonObject>* ProgramObject = nullptr;
	if (!RootObject->TryGetObjectField(TEXT("manifest"), ManifestObject)
		|| !RootObject->TryGetObjectField(TEXT("document"), DocumentObject)
		|| !RootObject->TryGetObjectField(TEXT("program"), ProgramObject))
	{
		if (OutError)
		{
			*OutError = TEXT("Logic Pack 必须包含 manifest、document 与 program 字段。");
		}
		return false;
	}

	FString Error;
	if (!DeserializeManifest(*ManifestObject, OutPack.Manifest, Error))
	{
		if (OutError)
		{
			*OutError = MoveTemp(Error);
		}
		return false;
	}

	// Document 复用既有 JSON 反序列化（含迁移与严格校验）。
	FString DocumentJson;
	const TSharedRef<TJsonWriter<>> DocumentWriter = TJsonWriterFactory<>::Create(&DocumentJson);
	if (!FJsonSerializer::Serialize(DocumentObject->ToSharedRef(), DocumentWriter))
	{
		if (OutError)
		{
			*OutError = TEXT("document: 无法重新序列化为 JSON。");
		}
		return false;
	}
	if (!FAkUGCDocumentJson::Deserialize(DocumentJson, OutPack.Document, &Error))
	{
		if (OutError)
		{
			*OutError = FString::Printf(TEXT("document: %s"), *Error);
		}
		return false;
	}

	if (!DeserializeProgram(*ProgramObject, OutPack.Program, Error))
	{
		if (OutError)
		{
			*OutError = MoveTemp(Error);
		}
		return false;
	}

	const TSharedPtr<FJsonObject>* SignatureObject = nullptr;
	if (bHasSignature)
	{
		if (!RootObject->TryGetObjectField(TEXT("signature"), SignatureObject)
			|| !SignatureObject
			|| !DeserializeSignature(*SignatureObject, OutPack.Signature, Error))
		{
			if (OutError)
			{
				*OutError = MoveTemp(Error);
			}
			return false;
		}
	}

	return true;
}

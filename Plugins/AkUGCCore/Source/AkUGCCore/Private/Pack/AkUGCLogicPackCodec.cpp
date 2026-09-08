#include "Pack/AkUGCLogicPackCodec.h"

#include "Document/AkUGCDocumentJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Guid.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
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
		TArray<FString>& Out,
		FString& Error)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Obj->TryGetArrayField(Field, Values))
		{
			Error = FString::Printf(TEXT("manifest.%s: 字段必须为数组。"), Field);
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			if (!Value.IsValid() || Value->Type != EJson::String)
			{
				Error = FString::Printf(TEXT("manifest.%s: 数组元素必须为字符串。"), Field);
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
			|| SchemaVersion < 0.0)
		{
			Error = TEXT("manifest.schemaVersion: 必须为非负整数。");
			return false;
		}
		Out.SchemaVersion = static_cast<int32>(SchemaVersion);
		Out.TemplateId = FName(*TemplateIdStr);
		Out.ContentHash = ContentHash;

		const TArray<TSharedPtr<FJsonValue>>* CapabilityValues = nullptr;
		if (!Obj->TryGetArrayField(TEXT("capabilities"), CapabilityValues))
		{
			Error = TEXT("manifest.capabilities: 字段必须为数组。");
			return false;
		}
		for (const TSharedPtr<FJsonValue>& CapabilityValue : *CapabilityValues)
		{
			if (!CapabilityValue.IsValid() || CapabilityValue->Type != EJson::String)
			{
				Error = TEXT("manifest.capabilities: 数组元素必须为字符串。");
				return false;
			}
			Out.Capabilities.Add(FName(*CapabilityValue->AsString()));
		}

		if (!ReadStringArrayField(Obj, TEXT("assetDependencies"), Out.AssetDependencies, Error))
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
		const UEnum* OpcodeEnum = StaticEnum<EAkUGCLogicOpcode>();

		const TArray<TSharedPtr<FJsonValue>>* Instructions = nullptr;
		if (!Obj->TryGetArrayField(TEXT("instructions"), Instructions))
		{
			Error = TEXT("program.instructions: 字段必须为数组。");
			return false;
		}

		for (int32 InstructionIndex = 0; InstructionIndex < Instructions->Num(); ++InstructionIndex)
		{
			const TSharedPtr<FJsonValue>& Value = (*Instructions)[InstructionIndex];
			if (!Value.IsValid() || Value->Type != EJson::Object)
			{
				Error = FString::Printf(TEXT("program.instructions[%d]: 必须为对象。"), InstructionIndex);
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
			if (!I->TryGetArrayField(TEXT("successorIndices"), Successors))
			{
				Error = FString::Printf(TEXT("program.instructions[%d].successorIndices: 字段必须为数组。"), InstructionIndex);
				return false;
			}
			for (const TSharedPtr<FJsonValue>& Successor : *Successors)
			{
				if (!Successor.IsValid() || Successor->Type != EJson::Number)
				{
					Error = FString::Printf(TEXT("program.instructions[%d].successorIndices: 元素必须为整数。"), InstructionIndex);
					return false;
				}
				const double Number = Successor->AsNumber();
				if (!FMath::IsFinite(Number) || Number != FMath::TruncToDouble(Number))
				{
					Error = FString::Printf(TEXT("program.instructions[%d].successorIndices: 元素必须为整数。"), InstructionIndex);
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
		Out.GameStartEntryIndex = static_cast<int32>(GameStartEntryIndex);
		Out.WaveStartEntryIndex = static_cast<int32>(WaveStartEntryIndex);
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

	const bool bSucceeded = FJsonSerializer::Serialize(RootObject.ToSharedRef(), TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&OutJson, 0));
	if (!bSucceeded && OutError)
	{
		*OutError = TEXT("Failed to serialize Logic Pack.");
	}
	return bSucceeded;
}

bool FAkUGCLogicPackCodec::Deserialize(const FString& Json, FAkUGCLogicPack& OutPack, FString* OutError)
{
	OutPack = FAkUGCLogicPack{};

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

	return true;
}

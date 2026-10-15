#include "Validation/AkUGCInputBudget.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"

namespace AkUGCInputBudgetPrivate
{
	bool Fail(FString* OutError, const FString& Message)
	{
		if (OutError)
		{
			*OutError = Message;
		}
		return false;
	}

	const TCHAR* SafeLabel(const TCHAR* Label)
	{
		return Label && *Label ? Label : TEXT("Input");
	}

	struct FJsonContainer
	{
		bool bObject = false;
		TSet<FString> Fields;
	};

	bool ValidateJsonStructure(
		const FString& Json,
		const TCHAR* Label,
		FString* OutError)
	{
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		TArray<FJsonContainer> Containers;
		int32 TokenCount = 0;
		EJsonNotation Notation;
		while (Reader->ReadNext(Notation))
		{
			if (Notation == EJsonNotation::Error)
			{
				return Fail(
					OutError,
					FString::Printf(
						TEXT("%s JSON is malformed: %s"),
						SafeLabel(Label),
						*Reader->GetErrorMessage()));
			}
			if (++TokenCount > AkUGCInputBudget::MaxJsonTokens)
			{
				return Fail(
					OutError,
					FString::Printf(
						TEXT("%s exceeds the JSON token budget of %d."),
						SafeLabel(Label),
						AkUGCInputBudget::MaxJsonTokens));
			}

			const FString& Identifier = Reader->GetIdentifier();
			if (Identifier.Len() > AkUGCInputBudget::MaxJsonIdentifierCharacters)
			{
				return Fail(
					OutError,
					FString::Printf(
						TEXT("%s contains a JSON field name longer than %d characters."),
						SafeLabel(Label),
						AkUGCInputBudget::MaxJsonIdentifierCharacters));
			}
			if (!Containers.IsEmpty() && Containers.Last().bObject)
			{
				if (Containers.Last().Fields.Contains(Identifier))
				{
					return Fail(
						OutError,
						FString::Printf(
							TEXT("JSON field '%s' is duplicated."),
							*Identifier));
				}
				Containers.Last().Fields.Add(Identifier);
			}

			if (Notation == EJsonNotation::ObjectStart
				|| Notation == EJsonNotation::ArrayStart)
			{
				if (Containers.Num() >= AkUGCInputBudget::MaxJsonDepth)
				{
					return Fail(
						OutError,
						FString::Printf(
							TEXT("%s exceeds the JSON nesting budget of %d."),
							SafeLabel(Label),
							AkUGCInputBudget::MaxJsonDepth));
				}
				FJsonContainer& Container = Containers.AddDefaulted_GetRef();
				Container.bObject = Notation == EJsonNotation::ObjectStart;
			}
			else if (Notation == EJsonNotation::ObjectEnd
				|| Notation == EJsonNotation::ArrayEnd)
			{
				if (Containers.IsEmpty())
				{
					return Fail(
						OutError,
						FString::Printf(
							TEXT("%s JSON container structure is invalid."),
							SafeLabel(Label)));
				}
				Containers.Pop();
			}
		}

		if (!Reader->GetErrorMessage().IsEmpty())
		{
			return Fail(
				OutError,
				FString::Printf(
					TEXT("%s JSON is malformed: %s"),
					SafeLabel(Label),
					*Reader->GetErrorMessage()));
		}
		if (!Containers.IsEmpty())
		{
			return Fail(
				OutError,
				FString::Printf(
					TEXT("%s JSON container structure is invalid."),
					SafeLabel(Label)));
		}
		return true;
	}
}

bool AkUGCInputBudget::ValidateUtf8Text(
	const FString& Text,
	int64 MaxBytes,
	const TCHAR* Label,
	FString* OutError)
{
	using namespace AkUGCInputBudgetPrivate;
	if (MaxBytes <= 0
		|| Text.IsEmpty()
		|| static_cast<int64>(Text.Len()) > MaxBytes)
	{
		return Fail(
			OutError,
			FString::Printf(
				TEXT("%s must contain 1 to %lld UTF-8 bytes."),
				SafeLabel(Label),
				static_cast<long long>(MaxBytes)));
	}
	const FTCHARToUTF8 Utf8(*Text);
	if (Utf8.Length() <= 0
		|| static_cast<int64>(Utf8.Length()) > MaxBytes)
	{
		return Fail(
			OutError,
			FString::Printf(
				TEXT("%s must contain 1 to %lld UTF-8 bytes."),
				SafeLabel(Label),
				static_cast<long long>(MaxBytes)));
	}
	return true;
}

bool AkUGCInputBudget::ValidateJson(
	const FString& Json,
	int64 MaxBytes,
	const TCHAR* Label,
	FString* OutError)
{
	using namespace AkUGCInputBudgetPrivate;
	return ValidateUtf8Text(Json, MaxBytes, Label, OutError)
		&& ValidateJsonStructure(Json, Label, OutError);
}

bool AkUGCInputBudget::LoadTextFile(
	const FString& Path,
	int64 MaxBytes,
	const TCHAR* Label,
	FString& OutText,
	FString* OutError)
{
	using namespace AkUGCInputBudgetPrivate;
	OutText.Reset();
	if (Path.IsEmpty() || MaxBytes <= 0)
	{
		return Fail(
			OutError,
			FString::Printf(TEXT("%s path or byte budget is invalid."), SafeLabel(Label)));
	}

	TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*Path));
	const int64 Size = Reader ? Reader->TotalSize() : -1;
	if (Size <= 0 || Size > MaxBytes || Size > MAX_int32)
	{
		return Fail(
			OutError,
			FString::Printf(
				TEXT("%s is missing, empty, or exceeds the %lld-byte input budget."),
				SafeLabel(Label),
				static_cast<long long>(MaxBytes)));
	}

	TArray<uint8> Bytes;
	Bytes.SetNumUninitialized(static_cast<int32>(Size));
	Reader->Serialize(Bytes.GetData(), Size);
	if (Reader->IsError() || Reader->Tell() != Size)
	{
		return Fail(
			OutError,
			FString::Printf(TEXT("%s could not be read completely."), SafeLabel(Label)));
	}

	FFileHelper::BufferToString(OutText, Bytes.GetData(), Bytes.Num());
	if (!ValidateUtf8Text(OutText, MaxBytes, Label, OutError))
	{
		OutText.Reset();
		return false;
	}
	return true;
}

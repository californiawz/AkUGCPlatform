#pragma once

#include "CoreMinimal.h"

/**
 * Untrusted-input budgets shared by Creator, runtime import and network-assisted tools.
 *
 * Byte limits are measured after UTF-8 conversion. File readers also enforce the on-disk
 * byte limit before allocating or decoding the payload.
 */
namespace AkUGCInputBudget
{
	inline constexpr int64 MaxProjectDocumentBytes = 4 * 1024 * 1024;
	inline constexpr int64 MaxCommandTransactionBytes = 4 * 1024 * 1024;
	inline constexpr int64 MaxLogicPackBytes = 16 * 1024 * 1024;
	inline constexpr int64 MaxCopilotRequestBytes = 64 * 1024;
	inline constexpr int64 MaxCopilotResponseBytes = 256 * 1024;
	inline constexpr int64 MaxCopilotProposalBytes = 64 * 1024;
	inline constexpr int64 MaxCopilotExplanationBytes = 16 * 1024;

	inline constexpr int32 MaxJsonDepth = 32;
	inline constexpr int32 MaxJsonTokens = 131072;
	inline constexpr int32 MaxJsonIdentifierCharacters = 128;
	inline constexpr int32 MaxCommandsPerTransaction = 1024;
	inline constexpr int32 MaxCopilotUrlCharacters = 2048;
	inline constexpr int32 MaxCopilotApiKeyBytes = 4096;

	/** Rejects empty text or text whose UTF-8 representation exceeds MaxBytes. */
	AKUGCCORE_API bool ValidateUtf8Text(
		const FString& Text,
		int64 MaxBytes,
		const TCHAR* Label,
		FString* OutError = nullptr);

	/**
	 * Applies the UTF-8 byte budget plus bounded JSON depth/token/identifier checks and
	 * duplicate-field rejection before a DOM parser is allowed to allocate the object tree.
	 */
	AKUGCCORE_API bool ValidateJson(
		const FString& Json,
		int64 MaxBytes,
		const TCHAR* Label,
		FString* OutError = nullptr);

	/** Reads through one file handle after checking its size, then verifies decoded UTF-8 size. */
	AKUGCCORE_API bool LoadTextFile(
		const FString& Path,
		int64 MaxBytes,
		const TCHAR* Label,
		FString& OutText,
		FString* OutError = nullptr);
}

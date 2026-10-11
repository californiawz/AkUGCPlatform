#pragma once

#include "CoreMinimal.h"
#include "Pack/AkUGCLogicPackLoader.h"

struct AKUGCCORE_API FAkUGCWorkEntry
{
    FGuid ReleaseId;
    FString DisplayName;
    FString ContentHash;
    FString TrustedPublicKey;
};

// The library directory is a local trust boundary, not an untrusted share container.
// Only .akugc files should cross that boundary through Import with an independently verified key.
class AKUGCCORE_API FAkUGCWorkLibrary
{
public:
    explicit FAkUGCWorkLibrary(const FString& Directory);
    static FString DefaultDirectory();
    static bool GetOrCreatePublisherKey(const FString& FilePath, FString& OutPrivateKey, FString* OutError = nullptr);
    bool Publish(const FAkUGCProjectDocument& Document, const FString& PrivateKey,
        const FString& ShareDirectory, FAkUGCWorkEntry& OutEntry, FString& OutSharePath, FString* OutError = nullptr) const;
    bool Import(const FString& SharePath, const FString& IndependentlyTrustedPublicKey,
        FAkUGCWorkEntry& OutEntry, FString* OutError = nullptr) const;
    bool List(TArray<FAkUGCWorkEntry>& OutEntries, int32& OutRejectedCount, FString* OutError = nullptr) const;
    FAkUGCLogicPackLoadResult Load(const FGuid& ReleaseId) const;
    bool Export(const FGuid& ReleaseId, const FString& Destination, FString* OutError = nullptr) const;
    bool Remove(const FGuid& ReleaseId, FString* OutError = nullptr) const;
    FString EntryPath(const FGuid& ReleaseId) const;
    static constexpr int64 MaxFileBytes = 16 * 1024 * 1024;

private:
    bool Store(const FString& Json, const FString& TrustedKey, FAkUGCWorkEntry& OutEntry, FString* OutError) const;
    FString Root;
};

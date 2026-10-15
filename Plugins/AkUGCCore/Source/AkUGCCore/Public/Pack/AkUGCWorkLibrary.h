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

    /** 产品发布入口：使用受保护的发布身份签名，不向调用方返回私钥。 */
    bool PublishWithPublisherIdentity(const FAkUGCProjectDocument& Document,
        const FString& PublisherDirectory, const FString& ShareDirectory,
        FAkUGCWorkEntry& OutEntry, FString& OutSharePath, FString* OutError = nullptr) const;

    /** 低层签名入口，供已有密钥的工具与测试夹具使用；不会把密钥写盘。 */
    bool Publish(const FAkUGCProjectDocument& Document, const FString& PrivateKey,
        const FString& ShareDirectory, FAkUGCWorkEntry& OutEntry, FString& OutSharePath, FString* OutError = nullptr) const;
    bool Import(const FString& SharePath, const FString& IndependentlyTrustedPublicKey,
        FAkUGCWorkEntry& OutEntry, FString* OutError = nullptr) const;
    /**
     * Imports exact downloaded bytes only after local signature verification and
     * binding the signed manifest to the requested Release and response ContentHash.
     */
    bool ImportArtifact(TConstArrayView<uint8> Artifact,
        const FString& IndependentlyTrustedPublicKey,
        const FGuid& ExpectedReleaseId, const FString& ExpectedContentHash,
        FAkUGCWorkEntry& OutEntry, FString* OutError = nullptr) const;
    bool List(TArray<FAkUGCWorkEntry>& OutEntries, int32& OutRejectedCount, FString* OutError = nullptr) const;
    FAkUGCLogicPackLoadResult Load(const FGuid& ReleaseId) const;
    bool Export(const FGuid& ReleaseId, const FString& Destination, FString* OutError = nullptr) const;
    bool Remove(const FGuid& ReleaseId, FString* OutError = nullptr) const;
    FString EntryPath(const FGuid& ReleaseId) const;
    static constexpr int64 MaxFileBytes = 16 * 1024 * 1024;

private:
    bool Store(const FString& Json, const FString& TrustedKey, FAkUGCWorkEntry& OutEntry, FString* OutError) const;
    bool CommitPublishedPack(const FAkUGCLogicPack& Pack, const FString& ShareDirectory,
        FAkUGCWorkEntry& OutEntry, FString& OutSharePath, FString* OutError) const;
    FString Root;
};

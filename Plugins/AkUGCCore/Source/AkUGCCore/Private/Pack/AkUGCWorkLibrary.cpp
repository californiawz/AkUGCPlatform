#include "Pack/AkUGCWorkLibrary.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Pack/AkUGCLogicPackBuilder.h"
#include "Pack/AkUGCLogicPackCodec.h"
#include "Pack/AkUGCLogicPackSignature.h"
#include "Pack/AkUGCPublisherKeyStore.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace AkUGCWorkFiles
{
    bool Fail(FString* Error, const FString& Message)
    {
        if (Error) { *Error = Message; }
        return false;
    }

    bool Read(const FString& Path, FString& Text, FString* Error, int64 Limit = FAkUGCWorkLibrary::MaxFileBytes)
    {
        TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*Path));
        const int64 Size = Reader ? Reader->TotalSize() : -1;
        if (Size <= 0 || Size > Limit) { return Fail(Error, TEXT("文件缺失、为空或超过大小限制。")); }
        TArray<uint8> Bytes;
        Bytes.SetNumUninitialized(static_cast<int32>(Size));
        Reader->Serialize(Bytes.GetData(), Size);
        if (Reader->IsError()) { return Fail(Error, TEXT("无法完整读取文件。")); }
        FFileHelper::BufferToString(Text, Bytes.GetData(), Bytes.Num());
        return true;
    }

    bool WriteNew(const FString& Path, const FString& Text, FString* Error)
    {
        if (Text.IsEmpty() || FTCHARToUTF8(*Text).Length() > FAkUGCWorkLibrary::MaxFileBytes)
        { return Fail(Error, TEXT("文件内容为空或超过大小限制。")); }
        if (IFileManager::Get().FileExists(*Path) || IFileManager::Get().DirectoryExists(*Path))
        { return Fail(Error, TEXT("目标已存在；不会覆盖已有文件。")); }
        if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true))
        { return Fail(Error, TEXT("无法创建目标目录。")); }
        const FString Temporary = Path + TEXT(".") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".tmp");
        const bool Written = FFileHelper::SaveStringToFile(Text, *Temporary, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
            && IFileManager::Get().Move(*Path, *Temporary, false, false, false, true);
        if (!Written) { IFileManager::Get().Delete(*Temporary); return Fail(Error, TEXT("文件原子写入失败。")); }
        return true;
    }

    FAkUGCWorkEntry Entry(const FAkUGCLogicPack& Pack, const FString& Key)
    {
        FAkUGCWorkEntry Result;
        Result.ReleaseId = Pack.Manifest.ReleaseId;
        Result.DisplayName = Pack.Document.Manifest.DisplayName;
        Result.ContentHash = Pack.Manifest.ContentHash;
        Result.TrustedPublicKey = Key;
        return Result;
    }
}

FAkUGCWorkLibrary::FAkUGCWorkLibrary(const FString& Directory)
    : Root(FPaths::ConvertRelativePathToFull(Directory)) {}

FString FAkUGCWorkLibrary::DefaultDirectory()
{
    return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UGC/Library"));
}

FString FAkUGCWorkLibrary::EntryPath(const FGuid& ReleaseId) const
{
    return ReleaseId.IsValid() ? FPaths::Combine(Root, ReleaseId.ToString(EGuidFormats::Digits) + TEXT(".ugcwork")) : FString();
}

bool FAkUGCWorkLibrary::Store(const FString& Json, const FString& TrustedKey, FAkUGCWorkEntry& OutEntry, FString* OutError) const
{
    const FString Key = TrustedKey.TrimStartAndEnd().ToLower();
    const auto Verified = FAkUGCLogicPackLoader::LoadVerified(Json, Key);
    if (!Verified.bSucceeded) { return AkUGCWorkFiles::Fail(OutError, Verified.ErrorMessage); }
    if (!Verified.Pack.Manifest.ReleaseId.IsValid()) { return AkUGCWorkFiles::Fail(OutError, TEXT("无效发布ID。")); }
    const FString Path = EntryPath(Verified.Pack.Manifest.ReleaseId);
    if (IFileManager::Get().FileExists(*Path))
    {
        const auto Existing = Load(Verified.Pack.Manifest.ReleaseId);
        FString ExistingJson, CandidateJson;
        if (!Existing.bSucceeded || Existing.Pack.Signature.PublicKey != Key
            || !FAkUGCLogicPackCodec::Serialize(Existing.Pack, ExistingJson)
            || !FAkUGCLogicPackCodec::Serialize(Verified.Pack, CandidateJson) || ExistingJson != CandidateJson)
        { return AkUGCWorkFiles::Fail(OutError, TEXT("发布ID冲突，已有作品未修改。")); }
        OutEntry = AkUGCWorkFiles::Entry(Verified.Pack, Key);
        return true;
    }
    TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetNumberField(TEXT("version"), 1);
    Object->SetStringField(TEXT("trustedPublicKey"), Key);
    Object->SetStringField(TEXT("packJson"), Json);
    FString Envelope;
    if (!FJsonSerializer::Serialize(Object, TJsonWriterFactory<>::Create(&Envelope))
        || !AkUGCWorkFiles::WriteNew(Path, Envelope, OutError)) { return false; }
    OutEntry = AkUGCWorkFiles::Entry(Verified.Pack, Key);
    return true;
}

bool FAkUGCWorkLibrary::CommitPublishedPack(
    const FAkUGCLogicPack& Pack,
    const FString& ShareDirectory,
    FAkUGCWorkEntry& OutEntry,
    FString& OutSharePath,
    FString* OutError) const
{
    FString Json;
    if (!FAkUGCLogicPackCodec::Serialize(Pack, Json, OutError)) { return false; }
    const auto Verified = FAkUGCLogicPackLoader::LoadVerified(Json, Pack.Signature.PublicKey);
    if (!Verified.bSucceeded) { return AkUGCWorkFiles::Fail(OutError, Verified.ErrorMessage); }
    const FString Path = FPaths::Combine(ShareDirectory, Pack.Manifest.ReleaseId.ToString(EGuidFormats::Digits) + TEXT(".akugc"));
    if (!AkUGCWorkFiles::WriteNew(Path, Json, OutError)) { return false; }
    if (!Store(Json, Pack.Signature.PublicKey, OutEntry, OutError))
    {
        if (!IFileManager::Get().Delete(*Path, true, false, true) && OutError)
        { *OutError += TEXT(" 分享文件回滚失败，需手动清理：") + Path; }
        return false;
    }
    OutSharePath = FPaths::ConvertRelativePathToFull(Path);
    return true;
}

bool FAkUGCWorkLibrary::PublishWithPublisherIdentity(
    const FAkUGCProjectDocument& Document,
    const FString& PublisherDirectory,
    const FString& ShareDirectory,
    FAkUGCWorkEntry& OutEntry,
    FString& OutSharePath,
    FString* OutError) const
{
    OutSharePath.Reset();
    const auto Build = FAkUGCLogicPackBuilder::Build(Document);
    if (!Build.bSucceeded) { return AkUGCWorkFiles::Fail(OutError, Build.ErrorMessage); }
    FAkUGCLogicPack Pack = Build.Pack;
    const FAkUGCPublisherKeyStore Publisher(PublisherDirectory);
    if (!Publisher.Sign(Pack.Manifest, Pack.Signature, OutError)) { return false; }
    return CommitPublishedPack(Pack, ShareDirectory, OutEntry, OutSharePath, OutError);
}

bool FAkUGCWorkLibrary::Publish(const FAkUGCProjectDocument& Document, const FString& PrivateKey,
    const FString& ShareDirectory, FAkUGCWorkEntry& OutEntry, FString& OutSharePath, FString* OutError) const
{
    OutSharePath.Reset();
    const auto Build = FAkUGCLogicPackBuilder::Build(Document);
    if (!Build.bSucceeded) { return AkUGCWorkFiles::Fail(OutError, Build.ErrorMessage); }
    FAkUGCLogicPack Pack = Build.Pack;
    if (!FAkUGCLogicPackSigner::Sign(Pack.Manifest, PrivateKey, Pack.Signature, OutError)) { return false; }
    return CommitPublishedPack(Pack, ShareDirectory, OutEntry, OutSharePath, OutError);
}

bool FAkUGCWorkLibrary::Import(const FString& SharePath, const FString& IndependentlyTrustedPublicKey,
    FAkUGCWorkEntry& OutEntry, FString* OutError) const
{
    FString Json;
    return AkUGCWorkFiles::Read(SharePath, Json, OutError) && Store(Json, IndependentlyTrustedPublicKey, OutEntry, OutError);
}

FAkUGCLogicPackLoadResult FAkUGCWorkLibrary::Load(const FGuid& ReleaseId) const
{
    FAkUGCLogicPackLoadResult Result;
    if (!ReleaseId.IsValid()) { Result.ErrorMessage = TEXT("无效发布ID。"); return Result; }
    FString Envelope;
    if (!AkUGCWorkFiles::Read(EntryPath(ReleaseId), Envelope, &Result.ErrorMessage)) { return Result; }
    TSharedPtr<FJsonObject> Object;
    FString Key, Json;
    double Version = 0;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Envelope), Object) || !Object.IsValid()
        || !Object->TryGetNumberField(TEXT("version"), Version) || Version != 1
        || !Object->TryGetStringField(TEXT("trustedPublicKey"), Key) || !Object->TryGetStringField(TEXT("packJson"), Json))
    { Result.ErrorMessage = TEXT("作品库记录损坏或版本不兼容。"); return Result; }
    Result = FAkUGCLogicPackLoader::LoadVerified(Json, Key);
    if (Result.bSucceeded && Result.Pack.Manifest.ReleaseId != ReleaseId)
    { Result.bSucceeded = false; Result.ErrorMessage = TEXT("作品记录ID与文件名不一致。"); }
    return Result;
}

bool FAkUGCWorkLibrary::List(TArray<FAkUGCWorkEntry>& OutEntries, int32& OutRejectedCount, FString* OutError) const
{
    OutEntries.Reset(); OutRejectedCount = 0;
    if (IFileManager::Get().FileExists(*Root)) { return AkUGCWorkFiles::Fail(OutError, TEXT("作品库路径不是目录。")); }
    TArray<FString> Files;
    IFileManager::Get().FindFiles(Files, *FPaths::Combine(Root, TEXT("*.ugcwork")), true, false);
    Files.Sort();
    for (const FString& File : Files)
    {
        FGuid Id;
        if (!FGuid::ParseExact(FPaths::GetBaseFilename(File), EGuidFormats::Digits, Id)) { ++OutRejectedCount; continue; }
        const auto Loaded = Load(Id);
        if (!Loaded.bSucceeded) { ++OutRejectedCount; continue; }
        OutEntries.Add(AkUGCWorkFiles::Entry(Loaded.Pack, Loaded.Pack.Signature.PublicKey));
    }
    return true;
}

bool FAkUGCWorkLibrary::Export(const FGuid& ReleaseId, const FString& Destination, FString* OutError) const
{
    const auto Loaded = Load(ReleaseId);
    if (!Loaded.bSucceeded) { return AkUGCWorkFiles::Fail(OutError, Loaded.ErrorMessage); }
    FString Json;
    return FAkUGCLogicPackCodec::Serialize(Loaded.Pack, Json, OutError) && AkUGCWorkFiles::WriteNew(Destination, Json, OutError);
}

bool FAkUGCWorkLibrary::Remove(const FGuid& ReleaseId, FString* OutError) const
{
    if (!ReleaseId.IsValid()) { return AkUGCWorkFiles::Fail(OutError, TEXT("无效发布ID。")); }
    return IFileManager::Get().Delete(*EntryPath(ReleaseId), true, false, true)
        || AkUGCWorkFiles::Fail(OutError, TEXT("无法删除作品库记录。"));
}

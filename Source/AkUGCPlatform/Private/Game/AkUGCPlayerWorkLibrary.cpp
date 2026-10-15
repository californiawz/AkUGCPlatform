#include "Game/AkUGCPlayerController.h"

#include "Engine/World.h"
#include "Game/AkUGCGameMode.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Pack/AkUGCWorkLibrary.h"
#include "Release/AkUGCPlayerRegistryState.h"
#include "Release/AkUGCPlayerReleaseRegistryClient.h"

FString AAkUGCPlayerController::WorkLibraryDirectory()
{
    FString Directory;
    return FParse::Value(FCommandLine::Get(), TEXT("WorkLibrary="), Directory) ? Directory : FAkUGCWorkLibrary::DefaultDirectory();
}

void AAkUGCPlayerController::ListWorks()
{
    if (!IsLocalController()) { return; }
    TArray<FAkUGCWorkEntry> Entries;
    int32 Rejected = 0;
    FString Error;
    if (!FAkUGCWorkLibrary(WorkLibraryDirectory()).List(Entries, Rejected, &Error))
    { WorkLibraryStatus = Error; return; }
    WorkLibraryStatus = FString::Printf(TEXT("WORK LIBRARY: %d works, %d rejected | F7: NEXT, F8: PLAY"), Entries.Num(), Rejected);
    UE_LOG(LogTemp, Display, TEXT("[AkUGCWorkLibrary] %s"), *WorkLibraryStatus);
    for (const auto& Entry : Entries)
    { UE_LOG(LogTemp, Display, TEXT("[AkUGCWorkLibrary] Work=%s Name=%s"), *Entry.ReleaseId.ToString(EGuidFormats::Digits), *Entry.DisplayName); }
}

void AAkUGCPlayerController::ImportWork(const FString& SharePath, const FString& IndependentlyTrustedPublicKey)
{
    if (!IsLocalController()) { return; }
    FAkUGCWorkEntry Entry;
    FString Error;
    const bool Imported = FAkUGCWorkLibrary(WorkLibraryDirectory()).Import(SharePath, IndependentlyTrustedPublicKey, Entry, &Error);
    WorkLibraryStatus = Imported ? TEXT("Imported verified work: ") + Entry.ReleaseId.ToString(EGuidFormats::Digits) : Error;
    UE_LOG(LogTemp, Display, TEXT("[AkUGCWorkLibrary] Import=%d %s"), Imported, *WorkLibraryStatus);
}

void AAkUGCPlayerController::FetchRegistryWork(
    const FString& RegistryBaseUrl,
    const FString& BearerToken,
    const FString& ReleaseId,
    const FString& IndependentlyTrustedPublicKey)
{
    if (!IsLocalController() || !HasAuthority()
        || GetNetMode() != NM_Standalone)
    {
        return;
    }
    if (!RegistryState)
    {
        RegistryState = new FAkUGCPlayerRegistryState();
    }
    if (RegistryState->Request.IsValid())
    {
        RegistryState->FailureCode = TEXT("busy");
        WorkLibraryStatus =
            TEXT("已有 Registry 下载正在进行，请先等待或取消。");
        return;
    }
    AAkUGCGameMode* Mode =
        GetWorld()->GetAuthGameMode<AAkUGCGameMode>();
    if (Mode && Mode->HasAuthoritySession())
    {
        RegistryState->FailureCode = TEXT("session_active");
        WorkLibraryStatus =
            TEXT("对局进行中不能获取并替换待选作品。");
        return;
    }

    FGuid Id;
    FString TrustedKey;
    FString Error;
    if (!FGuid::Parse(ReleaseId, Id)
        || !Id.IsValid()
        || !FAkUGCPlayerReleaseRegistryClient::NormalizeTrustedPublicKey(
            IndependentlyTrustedPublicKey,
            TrustedKey,
            Error))
    {
        RegistryState->FailureCode = TEXT("invalid_configuration");
        WorkLibraryStatus = Error.IsEmpty()
            ? TEXT("Release ID 无效。")
            : Error;
        UE_LOG(
            LogTemp,
            Warning,
            TEXT("[AkUGCPlayerRegistry] Rejected Work=%s Code=%s Detail=%s"),
            *ReleaseId,
            *RegistryState->FailureCode,
            *WorkLibraryStatus);
        return;
    }

    FString ExpectedUrl;
    FAkUGCHttpRequestPtr Request =
        FAkUGCPlayerReleaseRegistryClient::StartDownload(
            RegistryBaseUrl,
            BearerToken,
            Id,
            60.0f,
            ExpectedUrl,
            Error);
    if (!Request.IsValid())
    {
        RegistryState->FailureCode = TEXT("start_failed");
        WorkLibraryStatus = Error;
        UE_LOG(
            LogTemp,
            Warning,
            TEXT("[AkUGCPlayerRegistry] Rejected Work=%s Code=%s Detail=%s"),
            *ReleaseId,
            *RegistryState->FailureCode,
            *WorkLibraryStatus);
        return;
    }

    RegistryState->Request = MoveTemp(Request);
    RegistryState->ExpectedReleaseId = Id;
    RegistryState->ExpectedUrl = MoveTemp(ExpectedUrl);
    RegistryState->IndependentlyTrustedPublicKey = MoveTemp(TrustedKey);
    RegistryState->FailureCode.Reset();
    WorkLibraryStatus = TEXT("正在从 Registry 获取 published 作品...");
    UE_LOG(
        LogTemp,
        Display,
        TEXT("[AkUGCPlayerRegistry] Started Work=%s"),
        *Id.ToString(EGuidFormats::Digits));
}

void AAkUGCPlayerController::CancelRegistryWork()
{
    if (!IsLocalController() || !RegistryState
        || !RegistryState->Request.IsValid())
    {
        return;
    }
    RegistryState->Request->Cancel();
    WorkLibraryStatus = TEXT("正在取消 Registry 下载...");
}

void AAkUGCPlayerController::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (!RegistryState
        || !RegistryState->Request.IsValid()
        || !RegistryState->Request->IsDone())
    {
        return;
    }

    const FAkUGCHttpRequestPtr Completed = RegistryState->Request;
    RegistryState->Request.Reset();
    FAkUGCWorkEntry Entry;
    FString ArtifactSha256;
    FString FailureCode;
    FString Error;
    const bool bImported =
        FAkUGCPlayerReleaseRegistryClient::ImportDownloaded(
            Completed->TakeResult(),
            RegistryState->ExpectedUrl,
            RegistryState->ExpectedReleaseId,
            RegistryState->IndependentlyTrustedPublicKey,
            WorkLibraryDirectory(),
            Entry,
            ArtifactSha256,
            FailureCode,
            Error);
    RegistryState->ExpectedUrl.Reset();
    RegistryState->IndependentlyTrustedPublicKey.Reset();
    if (!bImported)
    {
        RegistryState->FailureCode = MoveTemp(FailureCode);
        WorkLibraryStatus = MoveTemp(Error);
        UE_LOG(
            LogTemp,
            Warning,
            TEXT("[AkUGCPlayerRegistry] Rejected Work=%s Code=%s Detail=%s"),
            *RegistryState->ExpectedReleaseId.ToString(
                EGuidFormats::Digits),
            *RegistryState->FailureCode,
            *WorkLibraryStatus);
        RegistryState->ExpectedReleaseId.Invalidate();
        return;
    }

    RegistryState->FailureCode.Reset();
    RegistryState->ExpectedReleaseId.Invalidate();
    SelectedWorkId =
        Entry.ReleaseId.ToString(EGuidFormats::Digits);
    WorkLibraryStatus =
        TEXT("已获取并本地验签：") + SelectedWorkId;
    UE_LOG(
        LogTemp,
        Display,
        TEXT("[AkUGCPlayerRegistry] Imported Work=%s ArtifactSHA256=%s"),
        *SelectedWorkId,
        *ArtifactSha256);
}

void AAkUGCPlayerController::EndPlay(
    const EEndPlayReason::Type EndPlayReason)
{
    if (RegistryState && RegistryState->Request.IsValid())
    {
        RegistryState->Request->Cancel();
        RegistryState->Request.Reset();
    }
    Super::EndPlay(EndPlayReason);
}

bool AAkUGCPlayerController::HasPendingRegistryWork() const
{
    return RegistryState && RegistryState->Request.IsValid();
}

FString AAkUGCPlayerController::GetRegistryFailureCode() const
{
    return RegistryState
        ? RegistryState->FailureCode
        : FString(TEXT("unavailable"));
}

void AAkUGCPlayerController::ExportWork(const FString& ReleaseId, const FString& Destination)
{
    if (!IsLocalController()) { return; }
    FGuid Id;
    FString Error;
    const bool Exported = FGuid::ParseExact(ReleaseId, EGuidFormats::Digits, Id)
        && FAkUGCWorkLibrary(WorkLibraryDirectory()).Export(Id, Destination, &Error);
    WorkLibraryStatus = Exported ? TEXT("Exported share: ") + Destination : Error.IsEmpty() ? TEXT("Invalid work ID") : Error;
    UE_LOG(LogTemp, Display, TEXT("[AkUGCWorkLibrary] Export=%d %s"), Exported, *WorkLibraryStatus);
}

void AAkUGCPlayerController::DeleteWork(const FString& ReleaseId)
{
    if (!IsLocalController()) { return; }
    FGuid Id;
    FString Error;
    const bool Removed = FGuid::ParseExact(ReleaseId, EGuidFormats::Digits, Id)
        && FAkUGCWorkLibrary(WorkLibraryDirectory()).Remove(Id, &Error);
    WorkLibraryStatus = Removed ? TEXT("Deleted local record: ") + ReleaseId : Error.IsEmpty() ? TEXT("Invalid work ID") : Error;
    UE_LOG(LogTemp, Display, TEXT("[AkUGCWorkLibrary] Delete=%d %s"), Removed, *WorkLibraryStatus);
}

void AAkUGCPlayerController::PlayWork(const FString& ReleaseId)
{
    if (!IsLocalController() || !HasAuthority()) { return; }
    FGuid Id;
    FString Error;
    AAkUGCGameMode* Mode = GetWorld()->GetAuthGameMode<AAkUGCGameMode>();
    const bool Prepared = FGuid::ParseExact(ReleaseId, EGuidFormats::Digits, Id) && Mode
        && Mode->PrepareLibraryWork(Id, WorkLibraryDirectory(), &Error);
    WorkLibraryStatus = Prepared ? TEXT("Verified work prepared | ENTER: START") : Error.IsEmpty() ? TEXT("Invalid work ID") : Error;
    if (Prepared) { SelectedWorkId = ReleaseId; }
    UE_LOG(LogTemp, Display, TEXT("[AkUGCWorkLibrary] Prepared=%d Work=%s %s"), Prepared, *ReleaseId, *WorkLibraryStatus);
}

void AAkUGCPlayerController::NextWork()
{
    if (!IsLocalController()) { return; }
    TArray<FAkUGCWorkEntry> Entries;
    int32 Rejected = 0;
    FAkUGCWorkLibrary(WorkLibraryDirectory()).List(Entries, Rejected);
    if (Entries.IsEmpty()) { ListWorks(); SelectedWorkId.Reset(); return; }
    const int32 Index = Entries.IndexOfByPredicate([this](const FAkUGCWorkEntry& Entry)
        { return Entry.ReleaseId.ToString(EGuidFormats::Digits) == SelectedWorkId; });
    const auto& Entry = Entries[(Index + 1) % Entries.Num()];
    SelectedWorkId = Entry.ReleaseId.ToString(EGuidFormats::Digits);
    WorkLibraryStatus = Entry.DisplayName + TEXT("\n") + SelectedWorkId + TEXT("\nF7: NEXT | F8: PREPARE SELECTED | ENTER: START");
}

void AAkUGCPlayerController::PlaySelectedWork()
{
    if (SelectedWorkId.IsEmpty()) { NextWork(); }
    if (!SelectedWorkId.IsEmpty()) { PlayWork(SelectedWorkId); }
}

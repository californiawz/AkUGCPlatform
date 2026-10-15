#include "Game/AkUGCPlayerController.h"

#include "Engine/World.h"
#include "Game/AkUGCGameMode.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Pack/AkUGCWorkLibrary.h"
#include "Release/AkUGCPlayerRegistryState.h"
#include "Release/AkUGCPlayerReleaseRegistryClient.h"

namespace
{
    void SecureResetString(FString& Value)
    {
        TArray<TCHAR>& Characters = Value.GetCharArray();
        if (!Characters.IsEmpty())
        {
            FMemory::Memzero(
                Characters.GetData(),
                Characters.Num() * sizeof(TCHAR));
        }
        Value.Reset();
    }

    void ClearRegistryChain(FAkUGCPlayerRegistryState& State)
    {
        SecureResetString(State.BearerToken);
        State.RegistryBaseUrl.Reset();
        State.ExpectedUrl.Reset();
        State.IndependentlyTrustedPublicKey.Reset();
        State.ExpectedProjectId.Invalidate();
        State.ExpectedReleaseId.Invalidate();
        State.Stage = EAkUGCPlayerRegistryStage::None;
    }
}

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
    RegistryState->Stage = EAkUGCPlayerRegistryStage::Download;
    RegistryState->ExpectedProjectId.Invalidate();
    RegistryState->ExpectedReleaseId = Id;
    RegistryState->ExpectedUrl = MoveTemp(ExpectedUrl);
    RegistryState->RegistryBaseUrl.Reset();
    SecureResetString(RegistryState->BearerToken);
    RegistryState->IndependentlyTrustedPublicKey = MoveTemp(TrustedKey);
    RegistryState->FailureCode.Reset();
    WorkLibraryStatus = TEXT("正在从 Registry 获取 published 作品...");
    UE_LOG(
        LogTemp,
        Display,
        TEXT("[AkUGCPlayerRegistry] Started Work=%s"),
        *Id.ToString(EGuidFormats::Digits));
}

void AAkUGCPlayerController::FetchRegistryProjectWork(
    const FString& RegistryBaseUrl,
    const FString& BearerToken,
    const FString& ProjectId,
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
            TEXT("已有 Registry 请求正在进行，请先等待或取消。");
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
    if (!FGuid::Parse(ProjectId, Id)
        || !Id.IsValid()
        || !FAkUGCPlayerReleaseRegistryClient::NormalizeTrustedPublicKey(
            IndependentlyTrustedPublicKey,
            TrustedKey,
            Error))
    {
        RegistryState->FailureCode = TEXT("invalid_configuration");
        WorkLibraryStatus = Error.IsEmpty()
            ? TEXT("Project ID 无效。")
            : Error;
        return;
    }

    FString ExpectedUrl;
    FAkUGCHttpRequestPtr Request =
        FAkUGCPlayerReleaseRegistryClient::StartGetWorkPage(
            RegistryBaseUrl,
            BearerToken,
            Id,
            30.0f,
            ExpectedUrl,
            Error);
    if (!Request.IsValid())
    {
        RegistryState->FailureCode = TEXT("start_failed");
        WorkLibraryStatus = Error;
        return;
    }

    ClearRegistryChain(*RegistryState);
    RegistryState->Request = MoveTemp(Request);
    RegistryState->Stage = EAkUGCPlayerRegistryStage::WorkPage;
    RegistryState->ExpectedProjectId = Id;
    RegistryState->ExpectedUrl = MoveTemp(ExpectedUrl);
    RegistryState->RegistryBaseUrl = RegistryBaseUrl;
    RegistryState->BearerToken = BearerToken;
    RegistryState->IndependentlyTrustedPublicKey = MoveTemp(TrustedKey);
    RegistryState->FailureCode.Reset();
    WorkLibraryStatus =
        TEXT("正在读取 Registry 作品页并解析最新 published 版本...");
    UE_LOG(
        LogTemp,
        Display,
        TEXT("[AkUGCPlayerRegistry] Started Project=%s"),
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
    SecureResetString(RegistryState->BearerToken);
    RegistryState->RegistryBaseUrl.Reset();
    WorkLibraryStatus = TEXT("正在取消 Registry 请求...");
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
    FAkUGCHttpResult Result = Completed->TakeResult();
    if (RegistryState->Stage == EAkUGCPlayerRegistryStage::WorkPage)
    {
        const FGuid ProjectId = RegistryState->ExpectedProjectId;
        FAkUGCPlayerRegistryWorkPage Work;
        FString FailureCode;
        FString Error;
        if (!FAkUGCPlayerReleaseRegistryClient::ParseWorkPage(
                Result,
                RegistryState->ExpectedUrl,
                ProjectId,
                Work,
                FailureCode,
                Error))
        {
            RegistryState->FailureCode = MoveTemp(FailureCode);
            WorkLibraryStatus = MoveTemp(Error);
            ClearRegistryChain(*RegistryState);
            UE_LOG(
                LogTemp,
                Warning,
                TEXT("[AkUGCPlayerRegistry] Rejected Project=%s Code=%s Detail=%s"),
                *ProjectId.ToString(EGuidFormats::Digits),
                *RegistryState->FailureCode,
                *WorkLibraryStatus);
            return;
        }

        FString ExpectedUrl;
        FAkUGCHttpRequestPtr Download =
            FAkUGCPlayerReleaseRegistryClient::StartDownload(
                RegistryState->RegistryBaseUrl,
                RegistryState->BearerToken,
                Work.LatestReleaseId,
                60.0f,
                ExpectedUrl,
                Error);
        SecureResetString(RegistryState->BearerToken);
        RegistryState->RegistryBaseUrl.Reset();
        if (!Download.IsValid())
        {
            RegistryState->FailureCode = TEXT("start_failed");
            WorkLibraryStatus = Error;
            ClearRegistryChain(*RegistryState);
            return;
        }
        RegistryState->Request = MoveTemp(Download);
        RegistryState->Stage = EAkUGCPlayerRegistryStage::Download;
        RegistryState->ExpectedProjectId.Invalidate();
        RegistryState->ExpectedReleaseId = Work.LatestReleaseId;
        RegistryState->ExpectedUrl = MoveTemp(ExpectedUrl);
        RegistryState->FailureCode.Reset();
        WorkLibraryStatus = FString::Printf(
            TEXT("作品页已解析：%s（%s）；正在下载并本地验签..."),
            *Work.DisplayName,
            *Work.Visibility);
        return;
    }
    if (RegistryState->Stage != EAkUGCPlayerRegistryStage::Download)
    {
        RegistryState->FailureCode = TEXT("invalid_state");
        WorkLibraryStatus = TEXT("Registry 请求状态无效。");
        ClearRegistryChain(*RegistryState);
        return;
    }

    const FGuid ExpectedReleaseId = RegistryState->ExpectedReleaseId;
    FAkUGCWorkEntry Entry;
    FString ArtifactSha256;
    FString FailureCode;
    FString Error;
    const bool bImported =
        FAkUGCPlayerReleaseRegistryClient::ImportDownloaded(
            Result,
            RegistryState->ExpectedUrl,
            ExpectedReleaseId,
            RegistryState->IndependentlyTrustedPublicKey,
            WorkLibraryDirectory(),
            Entry,
            ArtifactSha256,
            FailureCode,
            Error);
    if (!bImported)
    {
        RegistryState->FailureCode = MoveTemp(FailureCode);
        WorkLibraryStatus = MoveTemp(Error);
        ClearRegistryChain(*RegistryState);
        UE_LOG(
            LogTemp,
            Warning,
            TEXT("[AkUGCPlayerRegistry] Rejected Work=%s Code=%s Detail=%s"),
            *ExpectedReleaseId.ToString(EGuidFormats::Digits),
            *RegistryState->FailureCode,
            *WorkLibraryStatus);
        return;
    }

    RegistryState->FailureCode.Reset();
    ClearRegistryChain(*RegistryState);
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
    if (RegistryState)
    {
        ClearRegistryChain(*RegistryState);
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

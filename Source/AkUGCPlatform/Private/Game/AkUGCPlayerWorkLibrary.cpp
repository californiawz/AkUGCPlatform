#include "Game/AkUGCPlayerController.h"

#include "Engine/World.h"
#include "Game/AkUGCGameMode.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Pack/AkUGCWorkLibrary.h"

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

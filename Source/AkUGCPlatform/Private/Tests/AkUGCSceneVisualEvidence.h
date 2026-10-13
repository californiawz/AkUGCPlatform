#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

#if WITH_DEV_AUTOMATION_TESTS
class UWorld;
class UStaticMeshComponent;

namespace AkUGCSceneVisual
{
    // Read-only final player camera, lights and projected official mesh bounds.
    TSharedRef<FJsonObject> Snapshot(UWorld* World);

    // Explicit opt-in only. Reads the game viewport, never changes camera, lighting or gameplay.
    // Three cross-frame captures per static target: visible -> hidden -> restored.
    class FCapture
    {
    public:
        explicit FCapture(const FString& InLabel);
        ~FCapture();
        bool IsEnabled() const { return bEnabled; }
        bool Update(UWorld* World);
        void Cancel();
        bool RecordEnemy(UWorld* World);
        bool RecordState(UWorld* World, const FString& Name);
        bool Succeeded() const { return bSucceeded; }
        const FString& GetError() const { return Error; }
        const FString& GetReportPath() const { return ReportPath; }
    private:
        bool Fail(const FString& Message);
        bool SaveReport();
        void Restore();
        void ReleaseScreenshot();
        void OnScreenshot(int32 Width, int32 Height, const TArray<FColor>& Pixels);
        FString Directory, ReportPath, Error;
        FString RequestPath, BitmapName;
        FDelegateHandle ScreenshotHandle;
        bool bWaitingScreenshot = false, bCaptureReady = false;
        uint64 RequestFrame = 0, CaptureFrame = 0;
        double RequestedAt = 0;
        bool bEnabled = false, bDone = false, bSucceeded = false;
        int32 TargetIndex = 0, Phase = 0;
        uint64 LastFrame = 0;
        double LastTime = 0;
        TWeakObjectPtr<UWorld> SampleWorld;
        TArray<TWeakObjectPtr<UStaticMeshComponent>> Targets;
        TWeakObjectPtr<UStaticMeshComponent> HiddenMesh;
        bool bOriginalVisible = false;
        TSharedPtr<FJsonObject> Report, Sample;
        FTransform OriginalTransform;
    };
}
#endif

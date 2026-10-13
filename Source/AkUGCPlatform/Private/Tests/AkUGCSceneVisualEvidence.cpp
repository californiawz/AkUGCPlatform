#include "AkUGCSceneVisualEvidence.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Camera/PlayerCameraManager.h"
#include <cfloat>
#include "Components/LightComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Entity/AkUGCRuntimeEntityActor.h"
#include "GameFramework/PlayerController.h"
#include "Game/AkUGCGameState.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "ImageUtils.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Prefab/AkUGCOfficialPrefabCatalog.h"
#include "Prefab/AkUGCPrefabRegistry.h"
#include "Serialization/JsonSerializer.h"
#include "UnrealClient.h"

namespace AkUGCSceneVisual
{
    namespace
    {
        TArray<TSharedPtr<FJsonValue>> Vector(FVector V)
        { return {MakeShared<FJsonValueNumber>(V.X), MakeShared<FJsonValueNumber>(V.Y), MakeShared<FJsonValueNumber>(V.Z)}; }
        TArray<TSharedPtr<FJsonValue>> Rect(double X0, double Y0, double X1, double Y1)
        { return {MakeShared<FJsonValueNumber>(X0), MakeShared<FJsonValueNumber>(Y0), MakeShared<FJsonValueNumber>(X1), MakeShared<FJsonValueNumber>(Y1)}; }
        bool StaticTarget(FName Type)
        { return Type == TEXT("official.environment.ground") || Type == TEXT("official.gameplay.base") || Type == TEXT("official.tower.basic"); }
        TSharedRef<FJsonObject> MeshInfo(AAkUGCRuntimeEntityActor* Actor, APlayerController* PC, FIntPoint Size)
        {
            const auto Data = MakeShared<FJsonObject>();
            auto* Mesh = Actor->VisualMesh.Get();
            Data->SetStringField(TEXT("entity_id"), Actor->GetEntityId().ToString(EGuidFormats::Digits));
            Data->SetStringField(TEXT("prefab"), Actor->GetPrefabId().ToString());
            Data->SetStringField(TEXT("mesh"), Mesh && Mesh->GetStaticMesh() ? Mesh->GetStaticMesh()->GetPathName() : TEXT(""));
            FAkUGCPrefabRegistry Registry; FAkUGCOfficialPrefabCatalog::RegisterTowerDefense(Registry);
            const FSoftObjectPath* Expected = Registry.ResolveAsset(Actor->GetPrefabId(), TEXT("Win64"));
            Data->SetBoolField(TEXT("official_mesh"), Expected && Mesh && Mesh->GetStaticMesh() && Mesh->GetStaticMesh()->GetPathName() == Expected->ToString());
            Data->SetBoolField(TEXT("registered"), Mesh && Mesh->IsRegistered());
            Data->SetBoolField(TEXT("visible"), Mesh && Mesh->IsVisible() && !Mesh->bHiddenInGame && !Actor->IsHidden());
            Data->SetBoolField(TEXT("component_replicated"), Mesh && Mesh->GetIsReplicated());
            Data->SetArrayField(TEXT("location"), Vector(Actor->GetActorLocation()));
            if (Mesh) { Data->SetArrayField(TEXT("up_vector"), Vector(Mesh->GetUpVector())); }
            bool Projected = PC && Mesh && Size.X > 0 && Size.Y > 0;
            double X0 = DBL_MAX, Y0 = DBL_MAX, X1 = -DBL_MAX, Y1 = -DBL_MAX;
            if (Mesh)
            {
                const FBox Box = Mesh->Bounds.GetBox();
                Data->SetArrayField(TEXT("bounds_min"), Vector(Box.Min)); Data->SetArrayField(TEXT("bounds_max"), Vector(Box.Max));
                for (int32 Corner = 0; Corner < 8 && Projected; ++Corner)
                {
                    FVector2D Screen;
                    const FVector Point((Corner & 1) ? Box.Max.X : Box.Min.X, (Corner & 2) ? Box.Max.Y : Box.Min.Y, (Corner & 4) ? Box.Max.Z : Box.Min.Z);
                    Projected &= PC->ProjectWorldLocationToScreen(Point, Screen, false);
                    if (Projected) { X0 = FMath::Min(X0, Screen.X); X1 = FMath::Max(X1, Screen.X); Y0 = FMath::Min(Y0, Screen.Y); Y1 = FMath::Max(Y1, Screen.Y); }
                }
            }
            Projected &= X1 > 0 && Y1 > 0 && X0 < Size.X && Y0 < Size.Y;
            Data->SetBoolField(TEXT("projected"), Projected);
            TArray<TSharedPtr<FJsonValue>> Shadows;
            if (Projected)
            {
                Data->SetArrayField(TEXT("mesh_roi"), Rect(FMath::Clamp(X0, 0.0, double(Size.X)), FMath::Clamp(Y0, 0.0, double(Size.Y)), FMath::Clamp(X1, 0.0, double(Size.X)), FMath::Clamp(Y1, 0.0, double(Size.Y))));
                // The same bounded silhouette envelope is used for static targets
                // and for excluding other entities (including moving enemies).
                // Component visibility is deliberately not required for the target:
                // its hidden capture must retain the identical predicted envelope.
                if (Actor->GetPrefabId() != TEXT("official.environment.ground") && Mesh->CastShadow)
                {
                    const FBox Box = Mesh->Bounds.GetBox();
                    UWorld* World = Actor->GetWorld();
                    for (TActorIterator<AAkUGCRuntimeEntityActor> GroundIt(World); GroundIt; ++GroundIt)
                    {
                        auto* Ground = GroundIt->VisualMesh.Get();
                        if (GroundIt->GetPrefabId() != TEXT("official.environment.ground") || !Ground || !Ground->GetStaticMesh()
                            || Ground->GetStaticMesh()->GetPathName() != TEXT("/Engine/BasicShapes/Plane.Plane")
                            || !Ground->IsRegistered() || !Ground->IsVisible() || Ground->bHiddenInGame || GroundIt->IsHidden()
                            || Ground->GetUpVector().Z < 0.999) { continue; }
                        const FBox GroundBox = Ground->Bounds.GetBox();
                        if (GroundBox.GetSize().Z > 1.0) { continue; }
                        const double GroundZ = GroundBox.GetCenter().Z;
                        if (Box.Min.Z < GroundZ - 0.01 || Box.Max.Z <= GroundZ) { continue; }
                        for (TActorIterator<AActor> LightIt(World); LightIt; ++LightIt)
                        {
                            TInlineComponentArray<UDirectionalLightComponent*> Lights; LightIt->GetComponents(Lights);
                            for (auto* Light : Lights)
                            {
                                if (!Light->IsRegistered() || !Light->IsVisible() || Light->bHiddenInGame || LightIt->IsHidden()
                                    || !FMath::IsFinite(Light->Intensity) || Light->Intensity <= 0 || !Light->CastShadows) { continue; }
                                const FVector Direction = Light->GetForwardVector().GetSafeNormal();
                                // At least ~14 degrees downward; no near-horizontal
                                // intersections, extrapolation or arbitrary screen pad.
                                if (Direction.ContainsNaN() || Direction.Z > -0.25) { continue; }
                                bool Valid = true;
                                double SX0 = DBL_MAX, SY0 = DBL_MAX, SX1 = -DBL_MAX, SY1 = -DBL_MAX;
                                TArray<TSharedPtr<FJsonValue>> WorldPoints, ScreenPoints;
                                for (int32 Corner = 0; Corner < 8; ++Corner)
                                {
                                    const FVector Point((Corner & 1) ? Box.Max.X : Box.Min.X, (Corner & 2) ? Box.Max.Y : Box.Min.Y, (Corner & 4) ? Box.Max.Z : Box.Min.Z);
                                    const double Distance = (GroundZ - Point.Z) / Direction.Z;
                                    const FVector Hit = Point + Direction * Distance;
                                    FVector2D Screen;
                                    if (!FMath::IsFinite(Distance) || Distance < -0.01 || Distance > 2000.0 || Hit.ContainsNaN()
                                        || Hit.X < GroundBox.Min.X || Hit.X > GroundBox.Max.X || Hit.Y < GroundBox.Min.Y || Hit.Y > GroundBox.Max.Y
                                        || !PC->ProjectWorldLocationToScreen(Hit, Screen, false)
                                        || !FMath::IsFinite(Screen.X) || !FMath::IsFinite(Screen.Y)) { Valid = false; break; }
                                    WorldPoints.Add(MakeShared<FJsonValueArray>(Vector(Hit)));
                                    ScreenPoints.Add(MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{MakeShared<FJsonValueNumber>(Screen.X), MakeShared<FJsonValueNumber>(Screen.Y)}));
                                    SX0 = FMath::Min(SX0, Screen.X); SX1 = FMath::Max(SX1, Screen.X);
                                    SY0 = FMath::Min(SY0, Screen.Y); SY1 = FMath::Max(SY1, Screen.Y);
                                }
                                if (!Valid || SX1 <= 0 || SY1 <= 0 || SX0 >= Size.X || SY0 >= Size.Y) { continue; }
                                const auto Shadow = MakeShared<FJsonObject>();
                                Shadow->SetStringField(TEXT("light_component"), Light->GetPathName());
                                Shadow->SetStringField(TEXT("light_class"), Light->GetClass()->GetName());
                                Shadow->SetBoolField(TEXT("light_registered"), true); Shadow->SetBoolField(TEXT("light_visible"), true);
                                Shadow->SetBoolField(TEXT("light_cast_shadows"), Light->CastShadows);
                                Shadow->SetNumberField(TEXT("light_intensity"), Light->Intensity);
                                Shadow->SetArrayField(TEXT("ray_direction"), Vector(Direction));
                                Shadow->SetStringField(TEXT("ground_entity_id"), GroundIt->GetEntityId().ToString(EGuidFormats::Digits));
                                Shadow->SetArrayField(TEXT("ground_bounds_min"), Vector(GroundBox.Min)); Shadow->SetArrayField(TEXT("ground_bounds_max"), Vector(GroundBox.Max));
                                Shadow->SetNumberField(TEXT("ground_z"), GroundZ); Shadow->SetNumberField(TEXT("max_ray_distance"), 2000.0);
                                Shadow->SetBoolField(TEXT("mesh_cast_shadow"), Mesh->CastShadow);
                                Shadow->SetArrayField(TEXT("world_points"), WorldPoints); Shadow->SetArrayField(TEXT("screen_points"), ScreenPoints);
                                Shadow->SetArrayField(TEXT("roi"), Rect(FMath::Clamp(SX0, 0.0, double(Size.X)), FMath::Clamp(SY0, 0.0, double(Size.Y)), FMath::Clamp(SX1, 0.0, double(Size.X)), FMath::Clamp(SY1, 0.0, double(Size.Y))));
                                Shadows.Add(MakeShared<FJsonValueObject>(Shadow));
                                X0 = FMath::Min(X0, SX0); X1 = FMath::Max(X1, SX1); Y0 = FMath::Min(Y0, SY0); Y1 = FMath::Max(Y1, SY1);
                            }
                        }
                    }
                }
                Data->SetArrayField(TEXT("roi"), Rect(FMath::Clamp(X0, 0.0, double(Size.X)), FMath::Clamp(Y0, 0.0, double(Size.Y)), FMath::Clamp(X1, 0.0, double(Size.X)), FMath::Clamp(Y1, 0.0, double(Size.Y))));
            }
            Data->SetArrayField(TEXT("shadows"), Shadows);
            return Data;
        }
        bool WriteJson(const TSharedRef<FJsonObject>& Data, const FString& Path)
        {
            FString Json; return FJsonSerializer::Serialize(Data, TJsonWriterFactory<>::Create(&Json))
                && FFileHelper::SaveStringToFile(Json, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
        }
    }

    TSharedRef<FJsonObject> Snapshot(UWorld* World)
    {
        const auto Data = MakeShared<FJsonObject>();
        auto* PC = World ? World->GetFirstPlayerController() : nullptr;
        auto* ViewportClient = World ? World->GetGameViewport() : nullptr;
        const FIntPoint Size = ViewportClient && ViewportClient->Viewport ? ViewportClient->Viewport->GetSizeXY() : FIntPoint::ZeroValue;
        Data->SetArrayField(TEXT("viewport"), {MakeShared<FJsonValueNumber>(Size.X), MakeShared<FJsonValueNumber>(Size.Y)});
        Data->SetNumberField(TEXT("frame"), double(GFrameCounter));
        Data->SetNumberField(TEXT("net_mode"), World ? World->GetNetMode() : -1);
        const auto Camera = MakeShared<FJsonObject>();
        Camera->SetStringField(TEXT("view_target"), PC && PC->GetViewTarget() ? PC->GetViewTarget()->GetPathName() : TEXT(""));
        Camera->SetBoolField(TEXT("local_controller"), PC && PC->IsLocalController());
        Camera->SetBoolField(TEXT("manager_valid"), PC && PC->PlayerCameraManager);
        if (PC && PC->PlayerCameraManager)
        {
            const auto* Manager = PC->PlayerCameraManager.Get();
            const FRotator R = Manager->GetCameraRotation();
            Camera->SetArrayField(TEXT("location"), Vector(Manager->GetCameraLocation()));
            Camera->SetArrayField(TEXT("rotation"), Vector(FVector(R.Pitch, R.Yaw, R.Roll)));
            Camera->SetNumberField(TEXT("fov"), Manager->GetFOVAngle());
        }
        Data->SetObjectField(TEXT("camera"), Camera);
        TArray<TSharedPtr<FJsonValue>> Lights, Meshes;
        if (World)
        {
            for (TActorIterator<AActor> It(World); It; ++It)
            {
                TInlineComponentArray<ULightComponent*> Components; It->GetComponents(Components);
                for (auto* Light : Components)
                {
                    const auto L = MakeShared<FJsonObject>(); L->SetStringField(TEXT("component"), Light->GetPathName());
                    L->SetStringField(TEXT("class"), Light->GetClass()->GetName()); L->SetNumberField(TEXT("intensity"), Light->Intensity);
                    L->SetBoolField(TEXT("registered"), Light->IsRegistered()); L->SetBoolField(TEXT("visible"), Light->IsVisible() && !It->IsHidden());
                    Lights.Add(MakeShared<FJsonValueObject>(L));
                }
            }
            for (TActorIterator<AAkUGCRuntimeEntityActor> It(World); It; ++It)
            { Meshes.Add(MakeShared<FJsonValueObject>(MeshInfo(*It, PC, Size))); }
        }
        Data->SetArrayField(TEXT("lights"), Lights); Data->SetArrayField(TEXT("meshes"), Meshes);
        // Conservative exclusions matching AkUGCHUD: score/preparation/room text,
        // settlement panel and bottom work-library text. Captures retain HUD unchanged.
        TArray<TSharedPtr<FJsonValue>> Exclusions{MakeShared<FJsonValueArray>(Rect(0, 0, FMath::Min(390, Size.X), 190)),
            MakeShared<FJsonValueArray>(Rect(0, FMath::Max(0, Size.Y - 115), Size.X, Size.Y))};
        const auto* State = World ? World->GetGameState<AAkUGCGameState>() : nullptr;
        if (State && State->GetMatchFlow() == EAkUGCMatchFlow::Preparation)
        { Exclusions.Add(MakeShared<FJsonValueArray>(Rect(0, 190, FMath::Min(620, Size.X), 350))); }
        if (State && State->GetScoreSnapshot().bSettled)
        { Exclusions.Add(MakeShared<FJsonValueArray>(Rect((Size.X - 440) / 2.0, FMath::Max(190.0, (Size.Y - 190) / 2.0 - 10), (Size.X + 440) / 2.0, FMath::Max(190.0, (Size.Y - 190) / 2.0 - 10) + 210))); }
        Data->SetArrayField(TEXT("hud_exclusions"), Exclusions);
        return Data;
    }

    FCapture::FCapture(const FString& InLabel)
    {
        FString Root;
        bEnabled = FParse::Value(FCommandLine::Get(), TEXT("AkUGCSceneVisual="), Root);
        if (bEnabled)
        {
            Directory = FPaths::Combine(FPaths::ConvertRelativePathToFull(Root), InLabel);
            ReportPath = FPaths::Combine(Directory, TEXT("report.json"));
            if (FParse::Param(FCommandLine::Get(), TEXT("nullrhi"))) { Fail(TEXT("Scene visual evidence requires native rendering, not NullRHI.")); }
            else if (IFileManager::Get().DirectoryExists(*Directory) || !IFileManager::Get().MakeDirectory(*Directory, true))
            { Fail(TEXT("Scene visual evidence requires a fresh writable output directory: ") + Directory); }
        }
    }
    FCapture::~FCapture() { ReleaseScreenshot(); Restore(); }
    void FCapture::Restore()
    {
        if (auto* Mesh = HiddenMesh.Get()) { Mesh->SetVisibility(bOriginalVisible, false); }
        HiddenMesh.Reset();
    }
    void FCapture::ReleaseScreenshot()
    {
        if (ScreenshotHandle.IsValid())
        { UGameViewportClient::OnScreenshotCaptured().Remove(ScreenshotHandle); ScreenshotHandle.Reset(); }
        // Never reset HUD or another caller's global request.
        if (bWaitingScreenshot && FScreenshotRequest::IsScreenshotRequested() && FScreenshotRequest::GetFilename() == RequestPath)
        { FScreenshotRequest::Reset(); }
        bWaitingScreenshot = false;
    }
    void FCapture::Cancel() { ReleaseScreenshot(); Restore(); }
    void FCapture::OnScreenshot(int32 Width, int32 Height, const TArray<FColor>& Pixels)
    {
        if (!bWaitingScreenshot || bDone) { return; }
        const FString ActualPath = FScreenshotRequest::GetFilename();
        if (ActualPath != RequestPath)
        {
            // DrawHUD can replace a request on the settlement frame. The engine's
            // delegate branch bypasses its usual file writer, so preserve that
            // known HUD capture explicitly; it must never become a scene sample.
            FString HudPath;
            if (FParse::Value(FCommandLine::Get(), TEXT("AkUGCHUDCapture="), HudPath)
                && FPaths::ConvertRelativePathToFull(ActualPath) == FPaths::ConvertRelativePathToFull(HudPath)
                && Width > 0 && Height > 0 && int64(Pixels.Num()) == int64(Width) * Height)
            {
                TArray64<uint8> Png;
                FImageUtils::PNGCompressImageArray(Width, Height, TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png);
                if (Png.IsEmpty() || !FFileHelper::SaveArrayToFile(Png, *HudPath))
                { Fail(TEXT("Cannot preserve the interfering settlement HUD screenshot.")); return; }
                ReleaseScreenshot();
                LastFrame = GFrameCounter; LastTime = FPlatformTime::Seconds();
                return;
            }
            Fail(TEXT("Another caller replaced the owned scene screenshot request: ") + ActualPath); return;
        }
        UWorld* World = SampleWorld.Get();
        auto* Mesh = Targets.IsValidIndex(TargetIndex) ? Targets[TargetIndex].Get() : nullptr;
        auto* Client = World ? World->GetGameViewport() : nullptr;
        auto* PC = World ? World->GetFirstPlayerController() : nullptr;
        if (!World || !Mesh || !Client || !Client->Viewport || !PC || !PC->IsLocalController() || !PC->PlayerCameraManager
            || !GEngine || GEngine->GameViewport.Get() != Client)
        { Fail(TEXT("Scene target/world/final camera disappeared during screenshot delivery.")); return; }
        const FIntPoint Size = Client->Viewport->GetSizeXY();
        if (Width <= 0 || Height <= 0 || Size != FIntPoint(Width, Height)
            || int64(Pixels.Num()) != int64(Width) * Height || GFrameCounter < RequestFrame)
        { Fail(TEXT("Screenshot delivery dimensions/frame do not match the native game viewport.")); return; }
        if (!Mesh->IsRegistered() || !Mesh->GetComponentTransform().Equals(OriginalTransform, 0.001)
            || Mesh->IsVisible() != (Phase != 1))
        { Fail(TEXT("Static target transform/visibility changed before screenshot delivery.")); return; }
        // Snapshot and projection must describe the actual Draw capture, not the
        // earlier latent Update that only scheduled the screenshot request.
        const auto State = Snapshot(World);
        const TCHAR* Names[] = {TEXT("visible"), TEXT("hidden"), TEXT("restored")};
        if (Phase == 0)
        { Sample->SetObjectField(TEXT("target"), MeshInfo(CastChecked<AAkUGCRuntimeEntityActor>(Mesh->GetOwner()), PC, Size)); }
        if (!FFileHelper::CreateBitmap(*FPaths::Combine(Directory, BitmapName), Width, Height, Pixels.GetData()))
        { Fail(TEXT("Cannot write delivered native screenshot pixels as BMP.")); return; }
        Sample->SetStringField(Names[Phase], BitmapName);
        Sample->SetStringField(FString(Names[Phase]) + TEXT("_request"), RequestPath);
        Sample->SetNumberField(FString(Names[Phase]) + TEXT("_request_frame"), double(RequestFrame));
        Sample->SetObjectField(FString(Names[Phase]) + TEXT("_state"), State);
        CaptureFrame = GFrameCounter; bCaptureReady = true;
        if (Phase == 1) { Restore(); }
        // Unbind immediately so a later HUD capture follows UE's normal writer.
        // UE still owns request reset after broadcast; never request from a callback.
        bWaitingScreenshot = false;
        ReleaseScreenshot();
    }
    bool FCapture::RecordEnemy(UWorld* World)
    {
        if (!bEnabled || (!Error.IsEmpty()) || !World) { return false; }
        const auto Data = Snapshot(World);
        for (const auto& Value : Data->GetArrayField(TEXT("meshes")))
        {
            const auto M = Value->AsObject();
            if (M->GetStringField(TEXT("prefab")) == TEXT("official.unit.basic_enemy") && M->GetBoolField(TEXT("official_mesh"))
                && M->GetBoolField(TEXT("registered")) && M->GetBoolField(TEXT("visible")) && M->GetBoolField(TEXT("projected")))
            { return WriteJson(Data, FPaths::Combine(Directory, TEXT("enemy.json"))); }
        }
        return false;
    }
    bool FCapture::RecordState(UWorld* World, const FString& Name)
    { return bEnabled && World && Error.IsEmpty() && WriteJson(Snapshot(World), FPaths::Combine(Directory, Name)); }
    bool FCapture::Fail(const FString& Message)
    {
        ReleaseScreenshot(); Restore(); Error = Message; bDone = true; bSucceeded = false;
        if (Report) { Report->SetBoolField(TEXT("capture_complete"), false); Report->SetStringField(TEXT("error"), Error); SaveReport(); }
        return true;
    }
    bool FCapture::SaveReport() { return Report && WriteJson(Report.ToSharedRef(), ReportPath); }
    bool FCapture::Update(UWorld* World)
    {
        if (!bEnabled || bDone) { return true; }
        if (!World || World->GetNetMode() == NM_DedicatedServer) { return Fail(TEXT("Visual sampling needs a local rendered game world.")); }
        if (Targets.IsEmpty())
        {
            Report = MakeShared<FJsonObject>(); Report->SetNumberField(TEXT("schema"), 1);
            Report->SetStringField(TEXT("method"), TEXT("native_viewport_visible_hidden_restored"));
            Report->SetStringField(TEXT("capture_backend"), TEXT("FScreenshotRequest.OnScreenshotCaptured"));
            Report->SetBoolField(TEXT("exposure_modified"), false);
            Report->SetBoolField(TEXT("capture_complete"), false); Report->SetBoolField(TEXT("visibility_restored"), false);
            Report->SetArrayField(TEXT("samples"), {}); Report->SetObjectField(TEXT("initial"), Snapshot(World));
            SampleWorld = World;
            TSet<FName> Types;
            for (TActorIterator<AAkUGCRuntimeEntityActor> It(World); It; ++It)
            {
                if (!StaticTarget(It->GetPrefabId())) { continue; }
                auto* Mesh = It->VisualMesh.Get();
                if (!Mesh || !Mesh->GetStaticMesh() || !Mesh->IsRegistered() || !Mesh->IsVisible() || Mesh->bHiddenInGame || It->IsHidden() || Mesh->GetIsReplicated())
                { return Fail(TEXT("Official static target missing, hidden or replicated VisualMesh.")); }
                Targets.Add(Mesh); Types.Add(It->GetPrefabId());
            }
            if (!Types.Contains(TEXT("official.environment.ground")) || !Types.Contains(TEXT("official.gameplay.base")) || !Types.Contains(TEXT("official.tower.basic")))
            { return Fail(TEXT("Scene visual gate requires official ground, base and tower targets.")); }
            LastFrame = GFrameCounter; LastTime = FPlatformTime::Seconds(); return false;
        }
        if (SampleWorld.Get() != World) { return Fail(TEXT("World changed during scene visual sampling.")); }
        auto* Mesh = Targets[TargetIndex].Get();
        auto* PC = World->GetFirstPlayerController();
        auto* Client = World->GetGameViewport();
        if (!Mesh || !PC || !PC->IsLocalController() || !PC->PlayerCameraManager || !Client || !Client->Viewport
            || !GEngine || GEngine->GameViewport.Get() != Client)
        { return Fail(TEXT("Target, final camera or owning native game viewport disappeared.")); }
        if (bWaitingScreenshot)
        {
            if (FPlatformTime::Seconds() - RequestedAt > 10.0)
            { return Fail(TEXT("Owned native screenshot delegate timed out.")); }
            if (!FScreenshotRequest::IsScreenshotRequested())
            { return Fail(TEXT("Scene screenshot request was consumed without an owned pixel callback.")); }
            if (FScreenshotRequest::GetFilename() != RequestPath)
            { return Fail(TEXT("Scene screenshot request was replaced before Draw.")); }
            return false;
        }
        if (bCaptureReady)
        {
            // Do not change visibility until UE has finished broadcasting/resetting
            // its request, and ensure even same-frame delivery advances an actual tick.
            if (GFrameCounter <= CaptureFrame) { return false; }
            ReleaseScreenshot(); bCaptureReady = false;
            if (Phase == 0)
            {
                bOriginalVisible = Mesh->IsVisible(); HiddenMesh = Mesh;
                Mesh->SetVisibility(false, false);
            }
            else if (Phase == 2)
            {
                if (Mesh->IsVisible() != bOriginalVisible) { return Fail(TEXT("Original target visibility was not restored.")); }
                TArray<TSharedPtr<FJsonValue>> Samples = Report->GetArrayField(TEXT("samples"));
                Samples.Add(MakeShared<FJsonValueObject>(Sample)); Report->SetArrayField(TEXT("samples"), Samples);
                ++TargetIndex;
                if (TargetIndex == Targets.Num())
                {
                    Report->SetBoolField(TEXT("visibility_restored"), true); Report->SetBoolField(TEXT("capture_complete"), true);
                    Report->SetObjectField(TEXT("final"), Snapshot(World));
                    if (!SaveReport()) { return Fail(TEXT("Cannot persist scene visual report.")); }
                    bDone = bSucceeded = true; return true;
                }
            }
            Phase = (Phase + 1) % 3; LastFrame = GFrameCounter; LastTime = FPlatformTime::Seconds(); return false;
        }
        // Keep strict pixel/restore gates. Exposure is not changed here; the new
        // real screenshot evidence must establish whether 0.3s is stable enough.
        if (GFrameCounter < LastFrame + 3 || FPlatformTime::Seconds() - LastTime < 0.3) { return false; }
        if (Phase > 0 && !Mesh->GetComponentTransform().Equals(OriginalTransform, 0.001))
        { return Fail(TEXT("Static target moved during differential capture.")); }
        if (FScreenshotRequest::IsScreenshotRequested()) { return false; }
        if (UGameViewportClient::OnScreenshotCaptured().IsBound())
        { return Fail(TEXT("Another screenshot delegate already owns native capture; refusing to intercept it.")); }
        auto* ConsoleObject = IConsoleManager::Get().FindConsoleObject(TEXT("r.ScreenshotDelegate"));
        const auto* CVar = ConsoleObject ? ConsoleObject->AsVariable() : nullptr;
        if (!CVar || CVar->GetInt() == 0)
        { return Fail(TEXT("r.ScreenshotDelegate must be enabled for scene evidence; no CVar was modified.")); }
        if (Phase == 0)
        { Sample = MakeShared<FJsonObject>(); OriginalTransform = Mesh->GetComponentTransform(); }
        const TCHAR* Names[] = {TEXT("visible"), TEXT("hidden"), TEXT("restored")};
        BitmapName = FString::Printf(TEXT("target_%03d_%s.bmp"), TargetIndex, Names[Phase]);
        RequestPath = FPaths::Combine(Directory, FString::Printf(TEXT("request_%03d_%s_%s.png"),
            TargetIndex, Names[Phase], *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
        ScreenshotHandle = UGameViewportClient::OnScreenshotCaptured().AddRaw(this, &FCapture::OnScreenshot);
        bWaitingScreenshot = true; RequestFrame = GFrameCounter; RequestedAt = FPlatformTime::Seconds();
        FScreenshotRequest::RequestScreenshot(RequestPath, false, false, false);
        if (FScreenshotRequest::GetFilename() != RequestPath)
        { return Fail(TEXT("Native screenshot request did not retain the unique owned filename.")); }
        return false;
    }
}
#endif

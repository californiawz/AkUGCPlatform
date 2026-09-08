#include "Subsystem/AkUGCEditorSubsystem.h"

#include "Command/AkUGCCommand.h"
#include "Containers/Ticker.h"
#include "Document/AkUGCDocumentJson.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Entity/AkUGCEntityBindingComponent.h"
#include "GameFramework/Actor.h"
#include "LevelEditor.h"
#include "HAL/FileManager.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Prefab/AkUGCOfficialPrefabCatalog.h"
#include "Prefab/AkUGCPrefabRegistry.h"
#include "Scene/AkUGCSceneRuntime.h"
#include "Session/AkUGCDocumentRuntimeSession.h"
#include "Validation/AkUGCDocumentValidator.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

namespace
{
    bool ReplaceProjectFileAtomically(const FString& TemporaryPath, const FString& DestinationPath)
    {
#if PLATFORM_WINDOWS
        return MoveFileExW(
            *TemporaryPath,
            *DestinationPath,
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
        IFileManager& FileManager = IFileManager::Get();
        const FString BackupPath = DestinationPath + TEXT(".backup");
        const bool bHadDestination = FileManager.FileExists(*DestinationPath);
        if (bHadDestination)
        {
            FileManager.Delete(*BackupPath, false, true, true);
            if (!FileManager.Move(*BackupPath, *DestinationPath, false, true, false, true))
            {
                return false;
            }
        }
        if (FileManager.Move(*DestinationPath, *TemporaryPath, false, true, false, true))
        {
            FileManager.Delete(*BackupPath, false, true, true);
            return true;
        }
        if (bHadDestination)
        {
            FileManager.Move(*DestinationPath, *BackupPath, false, true, false, true);
        }
        return false;
#endif
    }
}

void UAkUGCEditorSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);

    PrefabRegistry = MakeUnique<FAkUGCPrefabRegistry>();
    FString Error;
    if (!FAkUGCOfficialPrefabCatalog::RegisterTowerDefense(*PrefabRegistry, &Error))
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to register official UGC prefab catalog: %s"), *Error);
    }
    RegisterEditorDelegates();
}

void UAkUGCEditorSubsystem::Deinitialize()
{
    UnregisterEditorDelegates();
    CloseProject();
    PrefabRegistry.Reset();
    Super::Deinitialize();
}

bool UAkUGCEditorSubsystem::NewTowerDefenseProject(FString* OutError)
{
    FAkUGCProjectDocument NewDocument;
    NewDocument.Manifest.ProjectId = FGuid::NewGuid();
    NewDocument.Manifest.DisplayName = TEXT("Tower Defense Project");
    NewDocument.Manifest.TemplateId = TEXT("official.tower_defense");
    NewDocument.Manifest.Capabilities = {
        TEXT("world.spawn"),
        TEXT("rules.wave"),
        TEXT("combat.damage")};

    FAkUGCSceneDocument& Scene = NewDocument.Scenes.AddDefaulted_GetRef();
    Scene.SceneId = FGuid::NewGuid();
    Scene.DisplayName = TEXT("Main");
    return OpenDocument(MoveTemp(NewDocument), OutError);
}

bool UAkUGCEditorSubsystem::SaveProject(const FString& FilePath, FString* OutError) const
{
    if (!HasOpenProject())
    {
        if (OutError)
        {
            *OutError = TEXT("No UGC project is open.");
        }
        return false;
    }

    // 保存前显式校验，拒绝保存非法文档（Serialize 已不再隐式校验）。
    const FAkUGCValidationResult Validation = FAkUGCDocumentValidator::Validate(Document);
    if (!Validation.IsValid())
    {
        if (OutError)
        {
            const FAkUGCValidationIssue* FirstError = Validation.Issues.FindByPredicate([](const FAkUGCValidationIssue& Issue)
            {
                return Issue.Severity == EAkUGCValidationSeverity::Error;
            });
            *OutError = FirstError
                ? FString::Printf(TEXT("%s: %s"), *FirstError->Path, *FirstError->Message)
                : TEXT("UGC project document validation failed.");
        }
        return false;
    }

    FString Json;
    if (!FAkUGCDocumentJson::Serialize(Document, Json, OutError))
    {
        return false;
    }

    IFileManager& FileManager = IFileManager::Get();
    FileManager.MakeDirectory(*FPaths::GetPath(FilePath), true);
    const FString TemporaryPath = FString::Printf(
        TEXT("%s.tmp-%s"),
        *FilePath,
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    if (!FFileHelper::SaveStringToFile(Json, *TemporaryPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        if (OutError)
        {
            *OutError = FString::Printf(TEXT("Failed to write temporary UGC project file '%s'."), *TemporaryPath);
        }
        return false;
    }

    FString WrittenJson;
    FAkUGCProjectDocument VerifiedDocument;
    FString VerificationError;
    if (!FFileHelper::LoadFileToString(WrittenJson, *TemporaryPath)
        || !FAkUGCDocumentJson::Deserialize(WrittenJson, VerifiedDocument, &VerificationError))
    {
        FileManager.Delete(*TemporaryPath, false, true, true);
        if (OutError)
        {
            *OutError = FString::Printf(
                TEXT("Temporary UGC project verification failed: %s"),
                VerificationError.IsEmpty() ? TEXT("file could not be read") : *VerificationError);
        }
        return false;
    }

    if (!ReplaceProjectFileAtomically(TemporaryPath, FilePath))
    {
        FileManager.Delete(*TemporaryPath, false, true, true);
        if (OutError)
        {
            *OutError = FString::Printf(TEXT("Failed to atomically replace UGC project file '%s'."), *FilePath);
        }
        return false;
    }
    return true;
}

bool UAkUGCEditorSubsystem::LoadProject(const FString& FilePath, FString* OutError)
{
    FString Json;
    if (!FFileHelper::LoadFileToString(Json, *FilePath))
    {
        if (OutError)
        {
            *OutError = FString::Printf(TEXT("Failed to read UGC project from '%s'."), *FilePath);
        }
        return false;
    }

    FAkUGCProjectDocument NewDocument;
    if (!FAkUGCDocumentJson::Deserialize(Json, NewDocument, OutError))
    {
        return false;
    }
    return OpenDocument(MoveTemp(NewDocument), OutError);
}

void UAkUGCEditorSubsystem::CloseProject()
{
    bUpdatingEditorSelection = true;
    if (GEditor && !IsRunningCommandlet() && !FApp::IsUnattended())
    {
        GEditor->SelectNone(false, true, false);
    }
    bUpdatingEditorSelection = false;
    SelectedEntityId.Invalidate();
    PendingDetachedEntityIds.Reset();
    PendingDeletedEntityIds.Reset();
    bCapturingEditorActorDeletion = false;

    CommandService.Reset();
    Session.Reset();
    if (Runtime)
    {
        Runtime->Unload();
    }
    Runtime.Reset();
    Document = FAkUGCProjectDocument{};
    ActiveSceneId.Invalidate();
    ++DocumentRevision;
}

FAkUGCCommandExecutionResult UAkUGCEditorSubsystem::PlacePrefab(
    FName PrefabId,
    const FTransform& Transform,
    FGuid& OutEntityId)
{
    OutEntityId.Invalidate();
    if (!CommandService)
    {
        return NoSessionResult();
    }

    TGuardValue<bool> ApplyingGuard(bApplyingUGCTransaction, true);
    FAkUGCCommandExecutionResult Result = CommandService->PlacePrefab(PrefabId, Transform, OutEntityId);
    if (Result.bSucceeded)
    {
        ++DocumentRevision;
        if (!IsRunningCommandlet() && !FApp::IsUnattended())
        {
            SelectEntity(OutEntityId);
        }
    }
    return Result;
}

FAkUGCCommandExecutionResult UAkUGCEditorSubsystem::DeleteEntity(const FGuid& EntityId)
{
    return ExecuteDeleteEntities({EntityId}, TEXT("Delete entity"));
}

FAkUGCCommandExecutionResult UAkUGCEditorSubsystem::ExecuteDeleteEntities(
    const TSet<FGuid>& EntityIds,
    const FString& Label)
{
    if (!CommandService)
    {
        return NoSessionResult();
    }

    for (const FAkUGCSceneDocument& Scene : Document.Scenes)
    {
        for (const FAkUGCEntityRecord& Entity : Scene.Entities)
        {
            if (EntityIds.Contains(Entity.ParentEntityId) && !EntityIds.Contains(Entity.EntityId))
            {
                PendingDetachedEntityIds.Remove(Entity.EntityId);
            }
        }
    }

    TGuardValue<bool> ApplyingGuard(bApplyingUGCTransaction, true);
    FAkUGCCommandExecutionResult Result = CommandService->DeleteEntities(EntityIds, Label);
    if (Result.bSucceeded)
    {
        ++DocumentRevision;
        if (EntityIds.Contains(SelectedEntityId))
        {
            SelectedEntityId.Invalidate();
        }
    }
    return Result;
}

FAkUGCCommandExecutionResult UAkUGCEditorSubsystem::DuplicateEntity(
    const FGuid& SourceEntityId,
    FGuid& OutEntityId)
{
    OutEntityId.Invalidate();
    if (!CommandService)
    {
        return NoSessionResult();
    }

    TGuardValue<bool> ApplyingGuard(bApplyingUGCTransaction, true);
    FAkUGCCommandExecutionResult Result = CommandService->DuplicateEntity(
        SourceEntityId,
        FVector(100.0, 100.0, 0.0),
        OutEntityId);
    if (Result.bSucceeded)
    {
        ++DocumentRevision;
        if (!IsRunningCommandlet() && !FApp::IsUnattended())
        {
            SelectEntity(OutEntityId);
        }
    }
    return Result;
}

FAkUGCCommandExecutionResult UAkUGCEditorSubsystem::SetEntityTransform(
    const FGuid& EntityId,
    const FTransform& Transform)
{
    return SetEntityTransforms({{EntityId, Transform}}, TEXT("Move entity"));
}

FAkUGCCommandExecutionResult UAkUGCEditorSubsystem::SetEntityTransforms(
    const TMap<FGuid, FTransform>& Transforms,
    const FString& Label)
{
    if (!CommandService)
    {
        return NoSessionResult();
    }

    TGuardValue<bool> ApplyingGuard(bApplyingUGCTransaction, true);
    FAkUGCCommandExecutionResult Result = CommandService->SetEntityTransforms(Transforms, Label);
    if (Result.bSucceeded)
    {
        ++DocumentRevision;
    }
    return Result;
}

FAkUGCCommandExecutionResult UAkUGCEditorSubsystem::SetEntityParent(
    const FGuid& EntityId,
    const FGuid& ParentEntityId)
{
    if (!CommandService)
    {
        return NoSessionResult();
    }

    TGuardValue<bool> ApplyingGuard(bApplyingUGCTransaction, true);
    FAkUGCCommandExecutionResult Result = CommandService->SetEntityParent(EntityId, ParentEntityId);
    if (Result.bSucceeded)
    {
        ++DocumentRevision;
    }
    return Result;
}

FAkUGCCommandExecutionResult UAkUGCEditorSubsystem::SetEntityProperty(
    const FGuid& EntityId,
    FName ComponentTypeId,
    FName PropertyId,
    const FAkUGCValue& Value)
{
    if (!CommandService)
    {
        return NoSessionResult();
    }

    TGuardValue<bool> ApplyingGuard(bApplyingUGCTransaction, true);
    FAkUGCCommandExecutionResult Result = CommandService->SetEntityProperty(
        EntityId,
        ComponentTypeId,
        PropertyId,
        Value);
    if (Result.bSucceeded)
    {
        ++DocumentRevision;
    }
    return Result;
}

FAkUGCCommandExecutionResult UAkUGCEditorSubsystem::AddLogicNode(const FAkUGCLogicNode& Node)
{
    if (!CommandService)
    {
        return NoSessionResult();
    }

    TGuardValue<bool> ApplyingGuard(bApplyingUGCTransaction, true);
    FAkUGCCommandExecutionResult Result = CommandService->AddLogicNode(Node);
    if (Result.bSucceeded)
    {
        ++DocumentRevision;
    }
    return Result;
}

FAkUGCCommandExecutionResult UAkUGCEditorSubsystem::DeleteLogicNode(const FGuid& NodeId)
{
    if (!CommandService)
    {
        return NoSessionResult();
    }

    TGuardValue<bool> ApplyingGuard(bApplyingUGCTransaction, true);
    FAkUGCCommandExecutionResult Result = CommandService->DeleteLogicNode(NodeId);
    if (Result.bSucceeded)
    {
        ++DocumentRevision;
    }
    return Result;
}

FAkUGCCommandExecutionResult UAkUGCEditorSubsystem::UpdateLogicNode(const FAkUGCLogicNode& Node)
{
    if (!CommandService)
    {
        return NoSessionResult();
    }

    TGuardValue<bool> ApplyingGuard(bApplyingUGCTransaction, true);
    FAkUGCCommandExecutionResult Result = CommandService->UpdateLogicNode(Node);
    if (Result.bSucceeded)
    {
        ++DocumentRevision;
    }
    return Result;
}

FAkUGCCommandExecutionResult UAkUGCEditorSubsystem::SetLogicNodePosition(
    const FGuid& NodeId,
    float PositionX,
    float PositionY)
{
    if (!CommandService)
    {
        return NoSessionResult();
    }

    TGuardValue<bool> ApplyingGuard(bApplyingUGCTransaction, true);
    FAkUGCCommandExecutionResult Result = CommandService->SetLogicNodePosition(NodeId, PositionX, PositionY);
    if (Result.bSucceeded)
    {
        ++DocumentRevision;
    }
    return Result;
}

FAkUGCCommandExecutionResult UAkUGCEditorSubsystem::ConnectLogicNode(
    const FGuid& SourceNodeId,
    const FGuid& TargetNodeId)
{
    if (!CommandService)
    {
        return NoSessionResult();
    }

    TGuardValue<bool> ApplyingGuard(bApplyingUGCTransaction, true);
    FAkUGCCommandExecutionResult Result = CommandService->ConnectLogicNode(SourceNodeId, TargetNodeId);
    if (Result.bSucceeded)
    {
        ++DocumentRevision;
    }
    return Result;
}

FAkUGCCommandExecutionResult UAkUGCEditorSubsystem::DisconnectLogicNode(
    const FGuid& SourceNodeId,
    const FGuid& TargetNodeId)
{
    if (!CommandService)
    {
        return NoSessionResult();
    }

    TGuardValue<bool> ApplyingGuard(bApplyingUGCTransaction, true);
    FAkUGCCommandExecutionResult Result = CommandService->DisconnectLogicNode(SourceNodeId, TargetNodeId);
    if (Result.bSucceeded)
    {
        ++DocumentRevision;
    }
    return Result;
}

FAkUGCCommandExecutionResult UAkUGCEditorSubsystem::DeleteSelectedEntity()
{
    return SelectedEntityId.IsValid()
        ? DeleteEntity(SelectedEntityId)
        : FAkUGCCommandExecutionResult::Failure(TEXT("editor.selection"), TEXT("No UGC entity is selected."));
}

FAkUGCCommandExecutionResult UAkUGCEditorSubsystem::DuplicateSelectedEntity(FGuid& OutEntityId)
{
    return SelectedEntityId.IsValid()
        ? DuplicateEntity(SelectedEntityId, OutEntityId)
        : FAkUGCCommandExecutionResult::Failure(TEXT("editor.selection"), TEXT("No UGC entity is selected."));
}

FAkUGCCommandExecutionResult UAkUGCEditorSubsystem::Undo()
{
    if (!CommandService)
    {
        return NoSessionResult();
    }
    TGuardValue<bool> ApplyingGuard(bApplyingUGCTransaction, true);
    FAkUGCCommandExecutionResult Result = CommandService->Undo();
    if (Result.bSucceeded)
    {
        if (SelectedEntityId.IsValid() && !FindEntity(SelectedEntityId))
        {
            SelectedEntityId.Invalidate();
        }
        ++DocumentRevision;
    }
    return Result;
}

FAkUGCCommandExecutionResult UAkUGCEditorSubsystem::Redo()
{
    if (!CommandService)
    {
        return NoSessionResult();
    }
    TGuardValue<bool> ApplyingGuard(bApplyingUGCTransaction, true);
    FAkUGCCommandExecutionResult Result = CommandService->Redo();
    if (Result.bSucceeded)
    {
        if (SelectedEntityId.IsValid() && !FindEntity(SelectedEntityId))
        {
            SelectedEntityId.Invalidate();
        }
        ++DocumentRevision;
    }
    return Result;
}

bool UAkUGCEditorSubsystem::SelectEntity(const FGuid& EntityId)
{
    if (!Runtime || !GEditor || IsRunningCommandlet())
    {
        return false;
    }

    AActor* Actor = Runtime->FindActor(EntityId);
    if (!Actor)
    {
        return false;
    }

    TGuardValue<bool> SelectionGuard(bUpdatingEditorSelection, true);
    GEditor->SelectNone(false, true, false);
    GEditor->SelectActor(Actor, true, true, true, true);
    SelectedEntityId = EntityId;
    return true;
}

AActor* UAkUGCEditorSubsystem::FindRuntimeActor(const FGuid& EntityId) const
{
    return Runtime ? Runtime->FindActor(EntityId) : nullptr;
}

FGuid UAkUGCEditorSubsystem::GetSelectedEntityId() const
{
    return SelectedEntityId;
}

const FAkUGCEntityRecord* UAkUGCEditorSubsystem::FindEntity(const FGuid& EntityId) const
{
    for (const FAkUGCSceneDocument& Scene : Document.Scenes)
    {
        if (const FAkUGCEntityRecord* Entity = Scene.Entities.FindByPredicate([&EntityId](const FAkUGCEntityRecord& Candidate)
        {
            return Candidate.EntityId == EntityId;
        }))
        {
            return Entity;
        }
    }
    return nullptr;
}

const FAkUGCPrefabDefinition* UAkUGCEditorSubsystem::FindPrefabForEntity(const FGuid& EntityId) const
{
    const FAkUGCEntityRecord* Entity = FindEntity(EntityId);
    return Entity && PrefabRegistry ? PrefabRegistry->Find(Entity->PrefabId) : nullptr;
}

bool UAkUGCEditorSubsystem::HasOpenProject() const
{
    return Session.IsValid() && ActiveSceneId.IsValid();
}

bool UAkUGCEditorSubsystem::CanUndo() const
{
    return CommandService && CommandService->CanUndo();
}

bool UAkUGCEditorSubsystem::CanRedo() const
{
    return CommandService && CommandService->CanRedo();
}

const FAkUGCProjectDocument& UAkUGCEditorSubsystem::GetDocument() const
{
    return Document;
}

const FAkUGCPrefabRegistry& UAkUGCEditorSubsystem::GetPrefabRegistry() const
{
    check(PrefabRegistry);
    return *PrefabRegistry;
}

const FAkUGCLogicGraph* UAkUGCEditorSubsystem::GetLogicGraph() const
{
    const FAkUGCSceneDocument* Scene = Document.Scenes.FindByPredicate(
        [this](const FAkUGCSceneDocument& Candidate)
        {
            return Candidate.SceneId == ActiveSceneId;
        });
    return Scene ? &Scene->LogicGraph : nullptr;
}

FAkUGCValidationResult UAkUGCEditorSubsystem::ValidateLogicGraph() const
{
    const FAkUGCLogicGraph* Graph = GetLogicGraph();
    if (!Graph)
    {
        FAkUGCValidationResult Result;
        Result.AddError(TEXT("logicGraph"), TEXT("No active scene logic graph is available."));
        return Result;
    }
    return FAkUGCDocumentValidator::ValidateLogicGraph(*Graph);
}

uint64 UAkUGCEditorSubsystem::GetDocumentRevision() const
{
    return DocumentRevision;
}

FString UAkUGCEditorSubsystem::GetDefaultProjectPath() const
{
    return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UGCProjects/Phase0.json"));
}

bool UAkUGCEditorSubsystem::OpenDocument(FAkUGCProjectDocument&& NewDocument, FString* OutError)
{
    const FAkUGCValidationResult Validation = FAkUGCDocumentValidator::Validate(NewDocument);
    if (!Validation.IsValid())
    {
        const FAkUGCValidationIssue* FirstError = Validation.Issues.FindByPredicate([](const FAkUGCValidationIssue& Issue)
        {
            return Issue.Severity == EAkUGCValidationSeverity::Error;
        });
        if (OutError)
        {
            *OutError = FirstError
                ? FString::Printf(TEXT("%s: %s"), *FirstError->Path, *FirstError->Message)
                : TEXT("UGC project validation failed.");
        }
        return false;
    }
    if (NewDocument.Scenes.IsEmpty())
    {
        if (OutError)
        {
            *OutError = TEXT("UGC project must contain at least one scene.");
        }
        return false;
    }
    if (!PrefabRegistry)
    {
        if (OutError)
        {
            *OutError = TEXT("Prefab registry is not initialized.");
        }
        return false;
    }

    UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!EditorWorld)
    {
        if (OutError)
        {
            *OutError = TEXT("Editor world is not available.");
        }
        return false;
    }

    const FGuid NewSceneId = NewDocument.Scenes[0].SceneId;
    TUniquePtr<FAkUGCSceneRuntime> NewRuntime = MakeUnique<FAkUGCSceneRuntime>(EditorWorld);
    TUniquePtr<FAkUGCDocumentRuntimeSession> NewSession = MakeUnique<FAkUGCDocumentRuntimeSession>(
        *NewRuntime,
        *PrefabRegistry,
        NewSceneId,
        EAkUGCRuntimeSessionMode::Edit);

    TGuardValue<bool> ApplyingGuard(bApplyingUGCTransaction, true);
    const FAkUGCCommandExecutionResult Result = NewSession->Initialize(NewDocument);
    if (!Result.bSucceeded)
    {
        if (OutError)
        {
            *OutError = Result.ErrorMessage;
        }
        return false;
    }

    CloseProject();
    Document = MoveTemp(NewDocument);
    ActiveSceneId = NewSceneId;
    Runtime = MoveTemp(NewRuntime);
    Session = MoveTemp(NewSession);
    CommandService = MakeUnique<FAkUGCRuntimeCommandService>(
        Document,
        *Session,
        *PrefabRegistry,
        ActiveSceneId);
    ++DocumentRevision;
    return true;
}

bool UAkUGCEditorSubsystem::CreateSession(FString* OutError)
{
    if (!ActiveSceneId.IsValid())
    {
        if (OutError)
        {
            *OutError = TEXT("No active scene is selected.");
        }
        return false;
    }
    return OpenDocument(FAkUGCProjectDocument(Document), OutError);
}

FAkUGCCommandExecutionResult UAkUGCEditorSubsystem::NoSessionResult() const
{
    return FAkUGCCommandExecutionResult::Failure(TEXT("editor.session"), TEXT("No UGC project is open."));
}

void UAkUGCEditorSubsystem::RegisterEditorDelegates()
{
    FLevelEditorModule& LevelEditorModule = FModuleManager::LoadModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
    SelectionChangedHandle = LevelEditorModule.OnActorSelectionChanged().AddUObject(
        this,
        &UAkUGCEditorSubsystem::OnActorSelectionChanged);

    if (GEditor)
    {
        ActorsMovedHandle = GEditor->OnActorsMoved().AddUObject(
            this,
            &UAkUGCEditorSubsystem::OnActorsMoved);
    }
    if (GEngine)
    {
        LevelActorAttachedHandle = GEngine->OnLevelActorAttached().AddUObject(
            this,
            &UAkUGCEditorSubsystem::OnLevelActorAttached);
        LevelActorDetachedHandle = GEngine->OnLevelActorDetached().AddUObject(
            this,
            &UAkUGCEditorSubsystem::OnLevelActorDetached);
        LevelActorAddedHandle = GEngine->OnLevelActorAdded().AddUObject(
            this,
            &UAkUGCEditorSubsystem::OnLevelActorAdded);
        LevelActorDeletedHandle = GEngine->OnLevelActorDeleted().AddUObject(
            this,
            &UAkUGCEditorSubsystem::OnLevelActorDeleted);
    }
    DeleteActorsBeginHandle = FEditorDelegates::OnDeleteActorsBegin.AddUObject(
        this,
        &UAkUGCEditorSubsystem::OnDeleteActorsBegin);
    DeleteActorsEndHandle = FEditorDelegates::OnDeleteActorsEnd.AddUObject(
        this,
        &UAkUGCEditorSubsystem::OnDeleteActorsEnd);
    HierarchyTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateUObject(this, &UAkUGCEditorSubsystem::TickPendingHierarchyChanges));
}

void UAkUGCEditorSubsystem::UnregisterEditorDelegates()
{
    if (SelectionChangedHandle.IsValid() && FModuleManager::Get().IsModuleLoaded(TEXT("LevelEditor")))
    {
        FModuleManager::GetModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"))
            .OnActorSelectionChanged()
            .Remove(SelectionChangedHandle);
        SelectionChangedHandle.Reset();
    }

    if (ActorsMovedHandle.IsValid() && GEditor)
    {
        GEditor->OnActorsMoved().Remove(ActorsMovedHandle);
        ActorsMovedHandle.Reset();
    }
    if (GEngine)
    {
        if (LevelActorAttachedHandle.IsValid())
        {
            GEngine->OnLevelActorAttached().Remove(LevelActorAttachedHandle);
            LevelActorAttachedHandle.Reset();
        }
        if (LevelActorDetachedHandle.IsValid())
        {
            GEngine->OnLevelActorDetached().Remove(LevelActorDetachedHandle);
            LevelActorDetachedHandle.Reset();
        }
        if (LevelActorAddedHandle.IsValid())
        {
            GEngine->OnLevelActorAdded().Remove(LevelActorAddedHandle);
            LevelActorAddedHandle.Reset();
        }
        if (LevelActorDeletedHandle.IsValid())
        {
            GEngine->OnLevelActorDeleted().Remove(LevelActorDeletedHandle);
            LevelActorDeletedHandle.Reset();
        }
    }
    if (DeleteActorsBeginHandle.IsValid())
    {
        FEditorDelegates::OnDeleteActorsBegin.Remove(DeleteActorsBeginHandle);
        DeleteActorsBeginHandle.Reset();
    }
    if (DeleteActorsEndHandle.IsValid())
    {
        FEditorDelegates::OnDeleteActorsEnd.Remove(DeleteActorsEndHandle);
        DeleteActorsEndHandle.Reset();
    }
    if (HierarchyTickerHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(HierarchyTickerHandle);
        HierarchyTickerHandle.Reset();
    }
    PendingDetachedEntityIds.Reset();
    PendingDeletedEntityIds.Reset();
    bCapturingEditorActorDeletion = false;
}

void UAkUGCEditorSubsystem::OnActorSelectionChanged(
    const TArray<UObject*>& NewSelection,
    bool bForceRefresh)
{
    if (bUpdatingEditorSelection)
    {
        return;
    }

    SelectedEntityId.Invalidate();
    if (!Runtime)
    {
        return;
    }

    for (UObject* SelectedObject : NewSelection)
    {
        AActor* Actor = Cast<AActor>(SelectedObject);
        const UAkUGCEntityBindingComponent* Binding = Actor
            ? Actor->FindComponentByClass<UAkUGCEntityBindingComponent>()
            : nullptr;
        if (Binding && Runtime->FindActor(Binding->EntityId) == Actor)
        {
            SelectedEntityId = Binding->EntityId;
            return;
        }
    }
}

void UAkUGCEditorSubsystem::OnLevelActorAttached(AActor* Actor, const AActor* ParentActor)
{
    if (bApplyingUGCTransaction || !Session || !Runtime || !Actor)
    {
        return;
    }

    const UAkUGCEntityBindingComponent* Binding = Actor->FindComponentByClass<UAkUGCEntityBindingComponent>();
    if (!Binding || Runtime->FindActor(Binding->EntityId) != Actor)
    {
        return;
    }

    PendingDetachedEntityIds.Remove(Binding->EntityId);
    CommitActorHierarchyChange(Actor, ParentActor);
}

void UAkUGCEditorSubsystem::OnLevelActorDetached(AActor* Actor, const AActor* ParentActor)
{
    if (bApplyingUGCTransaction || !Session || !Runtime || !Actor)
    {
        return;
    }

    const UAkUGCEntityBindingComponent* Binding = Actor->FindComponentByClass<UAkUGCEntityBindingComponent>();
    if (Binding && Runtime->FindActor(Binding->EntityId) == Actor)
    {
        PendingDetachedEntityIds.Add(Binding->EntityId);
    }
}

void UAkUGCEditorSubsystem::OnLevelActorAdded(AActor* Actor)
{
    if (bApplyingUGCTransaction || !Session || !Runtime || !Actor)
    {
        return;
    }

    const UAkUGCEntityBindingComponent* Binding = Actor->FindComponentByClass<UAkUGCEntityBindingComponent>();
    if (!Binding || Runtime->FindActor(Binding->EntityId) == Actor)
    {
        return;
    }

    // Project Document remains authoritative until native Undo is integrated with UGC history.
    TGuardValue<bool> ApplyingGuard(bApplyingUGCTransaction, true);
    if (UWorld* World = Actor->GetWorld())
    {
        World->EditorDestroyActor(Actor, false);
    }
}

void UAkUGCEditorSubsystem::OnLevelActorDeleted(AActor* Actor)
{
    if (bApplyingUGCTransaction || !bCapturingEditorActorDeletion || !Session || !Runtime || !Actor)
    {
        return;
    }

    const UAkUGCEntityBindingComponent* Binding = Actor->FindComponentByClass<UAkUGCEntityBindingComponent>();
    if (!Binding || !Runtime->NotifyActorDeletedExternally(Binding->EntityId, Actor))
    {
        return;
    }

    PendingDetachedEntityIds.Remove(Binding->EntityId);
    PendingDeletedEntityIds.Add(Binding->EntityId);
}

void UAkUGCEditorSubsystem::OnDeleteActorsBegin()
{
    bCapturingEditorActorDeletion = Session.IsValid() && Runtime.IsValid() && !bApplyingUGCTransaction;
    if (bCapturingEditorActorDeletion)
    {
        PendingDeletedEntityIds.Reset();
    }
}

void UAkUGCEditorSubsystem::OnDeleteActorsEnd()
{
    const bool bWasCapturing = bCapturingEditorActorDeletion;
    bCapturingEditorActorDeletion = false;
    if (bWasCapturing && !PendingDeletedEntityIds.IsEmpty())
    {
        CommitPendingActorDeletions();
    }
}

bool UAkUGCEditorSubsystem::TickPendingHierarchyChanges(float DeltaTime)
{
    if (bApplyingUGCTransaction || !Session || !Runtime)
    {
        return true;
    }

    if (PendingDetachedEntityIds.IsEmpty())
    {
        return true;
    }

    TArray<FGuid> EntityIds = PendingDetachedEntityIds.Array();
    PendingDetachedEntityIds.Reset();
    for (const FGuid& EntityId : EntityIds)
    {
        if (AActor* Actor = Runtime->FindActor(EntityId))
        {
            CommitActorHierarchyChange(Actor, Actor->GetAttachParentActor());
        }
    }
    return true;
}

bool UAkUGCEditorSubsystem::CommitPendingActorDeletions()
{
    const TSet<FGuid> EntityIds = MoveTemp(PendingDeletedEntityIds);
    PendingDeletedEntityIds.Reset();
    if (EntityIds.IsEmpty())
    {
        return true;
    }

    const FAkUGCCommandExecutionResult Result = ExecuteDeleteEntities(
        EntityIds,
        EntityIds.Num() == 1 ? TEXT("Delete entity from viewport") : TEXT("Delete entities from viewport"));
    if (!Result.bSucceeded)
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to commit UGC actor deletion: %s"), *Result.ErrorMessage);
        RestoreActorHierarchyFromDocument();
        return false;
    }
    return true;
}

bool UAkUGCEditorSubsystem::CommitActorHierarchyChange(AActor* Actor, const AActor* ParentActor)
{
    if (!Actor || !Session || !Runtime)
    {
        return false;
    }

    const UAkUGCEntityBindingComponent* Binding = Actor->FindComponentByClass<UAkUGCEntityBindingComponent>();
    if (!Binding || Runtime->FindActor(Binding->EntityId) != Actor)
    {
        return false;
    }

    FGuid ParentEntityId;
    if (ParentActor)
    {
        const UAkUGCEntityBindingComponent* ParentBinding =
            ParentActor->FindComponentByClass<UAkUGCEntityBindingComponent>();
        if (!ParentBinding || Runtime->FindActor(ParentBinding->EntityId) != ParentActor)
        {
            UE_LOG(LogTemp, Warning, TEXT("UGC entities can only be attached to another UGC entity."));
            RestoreActorHierarchyFromDocument();
            return false;
        }
        ParentEntityId = ParentBinding->EntityId;
    }

    const FAkUGCEntityRecord* Entity = FindEntity(Binding->EntityId);
    if (!Entity)
    {
        RestoreActorHierarchyFromDocument();
        return false;
    }

    const FTransform CurrentTransform = Actor->GetActorTransform();
    const bool bTransformChanged = !Entity->Transform.Equals(CurrentTransform, 0.01);
    if (Entity->ParentEntityId == ParentEntityId && !bTransformChanged)
    {
        return true;
    }
    if (!CommandService)
    {
        RestoreActorHierarchyFromDocument();
        return false;
    }

    TGuardValue<bool> ApplyingGuard(bApplyingUGCTransaction, true);
    const FAkUGCCommandExecutionResult Result = CommandService->SetEntityParentAndTransform(
        Binding->EntityId,
        ParentEntityId,
        bTransformChanged ? TOptional<FTransform>(CurrentTransform) : TOptional<FTransform>{},
        ParentEntityId.IsValid() ? TEXT("Change entity parent") : TEXT("Detach entity from parent"));
    if (!Result.bSucceeded)
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to commit UGC hierarchy change: %s"), *Result.ErrorMessage);
        RestoreActorHierarchyFromDocument();
        return false;
    }

    ++DocumentRevision;
    return true;
}

void UAkUGCEditorSubsystem::RestoreActorHierarchyFromDocument()
{
    if (!Runtime || !PrefabRegistry)
    {
        return;
    }

    const FAkUGCSceneDocument* Scene = Document.Scenes.FindByPredicate([this](const FAkUGCSceneDocument& Candidate)
    {
        return Candidate.SceneId == ActiveSceneId;
    });
    if (!Scene)
    {
        return;
    }

    TGuardValue<bool> ApplyingGuard(bApplyingUGCTransaction, true);
    FString Error;
    if (!Runtime->SynchronizeScene(*Scene, *PrefabRegistry, &Error))
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to restore UGC hierarchy from document: %s"), *Error);
    }
}

void UAkUGCEditorSubsystem::OnActorsMoved(TArray<AActor*>& Actors)
{
    if (bApplyingUGCTransaction || !Session || !Runtime)
    {
        return;
    }

    TMap<FGuid, FTransform> ChangedTransforms;
    for (AActor* Actor : Actors)
    {
        AddActorAndUGCDescendants(Actor, ChangedTransforms);
    }
    if (ChangedTransforms.IsEmpty())
    {
        return;
    }

    const FAkUGCCommandExecutionResult Result = SetEntityTransforms(
        ChangedTransforms,
        ChangedTransforms.Num() == 1 ? TEXT("Move entity") : TEXT("Move entity hierarchy"));
    if (!Result.bSucceeded)
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to commit UGC actor movement: %s"), *Result.ErrorMessage);
    }
}

void UAkUGCEditorSubsystem::AddActorAndUGCDescendants(
    AActor* Actor,
    TMap<FGuid, FTransform>& OutTransforms) const
{
    if (!Actor || !Runtime)
    {
        return;
    }

    TArray<AActor*> CandidateActors;
    CandidateActors.Add(Actor);
    Actor->GetAttachedActors(CandidateActors, false, true);

    for (AActor* Candidate : CandidateActors)
    {
        const UAkUGCEntityBindingComponent* Binding = Candidate
            ? Candidate->FindComponentByClass<UAkUGCEntityBindingComponent>()
            : nullptr;
        if (!Binding || Runtime->FindActor(Binding->EntityId) != Candidate)
        {
            continue;
        }

        const FTransform CurrentTransform = Candidate->GetActorTransform();
        if (!Binding->SourceRecord.Transform.Equals(CurrentTransform, 0.01))
        {
            OutTransforms.Add(Binding->EntityId, CurrentTransform);
        }
    }
}

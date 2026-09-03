#include "Subsystem/AkUGCEditorSubsystem.h"

#include "Command/AkUGCCommand.h"
#include "Document/AkUGCDocumentJson.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
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

    FString Json;
    if (!FAkUGCDocumentJson::Serialize(Document, Json, OutError))
    {
        return false;
    }

    IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), true);
    if (!FFileHelper::SaveStringToFile(Json, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        if (OutError)
        {
            *OutError = FString::Printf(TEXT("Failed to save UGC project to '%s'."), *FilePath);
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

    Session.Reset();
    if (Runtime)
    {
        Runtime->Unload();
    }
    Runtime.Reset();
    Document = FAkUGCProjectDocument{};
    ActiveSceneId.Invalidate();
}

FAkUGCCommandExecutionResult UAkUGCEditorSubsystem::PlacePrefab(
    FName PrefabId,
    const FTransform& Transform,
    FGuid& OutEntityId)
{
    if (!Session || !PrefabRegistry)
    {
        return NoSessionResult();
    }

    FAkUGCEntityRecord Entity;
    OutEntityId = FGuid::NewGuid();
    FString Error;
    if (!PrefabRegistry->CreateEntityRecord(PrefabId, OutEntityId, Transform, Entity, &Error))
    {
        OutEntityId.Invalidate();
        return FAkUGCCommandExecutionResult::Failure(TEXT("editor.prefabId"), MoveTemp(Error));
    }

    FAkUGCCommand Command;
    Command.CommandId = FGuid::NewGuid();
    Command.Type = EAkUGCCommandType::AddEntity;
    Command.SceneId = ActiveSceneId;
    Command.EntityId = OutEntityId;
    Command.Entity = MoveTemp(Entity);

    FAkUGCCommandTransaction Transaction;
    Transaction.TransactionId = FGuid::NewGuid();
    Transaction.Label = FString::Printf(TEXT("Place %s"), *PrefabId.ToString());
    Transaction.Commands.Add(MoveTemp(Command));

    TGuardValue<bool> ApplyingGuard(bApplyingUGCTransaction, true);
    FAkUGCCommandExecutionResult Result = Session->Execute(Document, Transaction);
    if (Result.bSucceeded && !IsRunningCommandlet() && !FApp::IsUnattended())
    {
        SelectEntity(OutEntityId);
    }
    return Result;
}

FAkUGCCommandExecutionResult UAkUGCEditorSubsystem::DeleteEntity(const FGuid& EntityId)
{
    if (!Session)
    {
        return NoSessionResult();
    }

    FAkUGCCommand Command;
    Command.CommandId = FGuid::NewGuid();
    Command.Type = EAkUGCCommandType::DeleteEntity;
    Command.SceneId = ActiveSceneId;
    Command.EntityId = EntityId;

    FAkUGCCommandTransaction Transaction;
    Transaction.TransactionId = FGuid::NewGuid();
    Transaction.Label = TEXT("Delete entity");
    Transaction.Commands.Add(MoveTemp(Command));

    TGuardValue<bool> ApplyingGuard(bApplyingUGCTransaction, true);
    FAkUGCCommandExecutionResult Result = Session->Execute(Document, Transaction);
    if (Result.bSucceeded && SelectedEntityId == EntityId)
    {
        SelectedEntityId.Invalidate();
    }
    return Result;
}

FAkUGCCommandExecutionResult UAkUGCEditorSubsystem::DuplicateEntity(
    const FGuid& SourceEntityId,
    FGuid& OutEntityId)
{
    if (!Session || !Runtime)
    {
        return NoSessionResult();
    }

    AActor* SourceActor = Runtime->FindActor(SourceEntityId);
    if (!SourceActor)
    {
        return FAkUGCCommandExecutionResult::Failure(
            TEXT("editor.sourceEntityId"),
            TEXT("Selected UGC entity does not exist in the runtime scene."));
    }

    OutEntityId = FGuid::NewGuid();
    FAkUGCCommand Command;
    Command.CommandId = FGuid::NewGuid();
    Command.Type = EAkUGCCommandType::DuplicateEntity;
    Command.SceneId = ActiveSceneId;
    Command.EntityId = OutEntityId;
    Command.SourceEntityId = SourceEntityId;
    Command.Transform = SourceActor->GetActorTransform();
    Command.Transform.AddToTranslation(FVector(100.0, 100.0, 0.0));

    FAkUGCCommandTransaction Transaction;
    Transaction.TransactionId = FGuid::NewGuid();
    Transaction.Label = TEXT("Duplicate entity");
    Transaction.Commands.Add(MoveTemp(Command));

    TGuardValue<bool> ApplyingGuard(bApplyingUGCTransaction, true);
    FAkUGCCommandExecutionResult Result = Session->Execute(Document, Transaction);
    if (Result.bSucceeded && !IsRunningCommandlet() && !FApp::IsUnattended())
    {
        SelectEntity(OutEntityId);
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
    if (!Session)
    {
        return NoSessionResult();
    }
    if (Transforms.IsEmpty())
    {
        return FAkUGCCommandExecutionResult::Failure(
            TEXT("editor.transforms"),
            TEXT("At least one entity transform is required."));
    }

    TArray<FGuid> EntityIds;
    Transforms.GetKeys(EntityIds);
    EntityIds.Sort([](const FGuid& Left, const FGuid& Right)
    {
        return Left.ToString(EGuidFormats::Digits) < Right.ToString(EGuidFormats::Digits);
    });

    FAkUGCCommandTransaction Transaction;
    Transaction.TransactionId = FGuid::NewGuid();
    Transaction.Label = Label;
    for (const FGuid& EntityId : EntityIds)
    {
        FAkUGCCommand& Command = Transaction.Commands.AddDefaulted_GetRef();
        Command.CommandId = FGuid::NewGuid();
        Command.Type = EAkUGCCommandType::SetTransform;
        Command.SceneId = ActiveSceneId;
        Command.EntityId = EntityId;
        Command.Transform = Transforms.FindChecked(EntityId);
    }

    TGuardValue<bool> ApplyingGuard(bApplyingUGCTransaction, true);
    return Session->Execute(Document, Transaction);
}

FAkUGCCommandExecutionResult UAkUGCEditorSubsystem::SetEntityProperty(
    const FGuid& EntityId,
    FName ComponentTypeId,
    FName PropertyId,
    const FAkUGCValue& Value)
{
    if (!Session || !PrefabRegistry)
    {
        return NoSessionResult();
    }

    const FAkUGCPrefabDefinition* Prefab = FindPrefabForEntity(EntityId);
    if (!Prefab)
    {
        return FAkUGCCommandExecutionResult::Failure(TEXT("editor.entityId"), TEXT("Entity or prefab definition does not exist."));
    }

    const FAkUGCPropertyDefinition* Property = Prefab->EditableProperties.FindByPredicate(
        [ComponentTypeId, PropertyId](const FAkUGCPropertyDefinition& Candidate)
        {
            return Candidate.ComponentTypeId == ComponentTypeId && Candidate.PropertyId == PropertyId;
        });
    if (!Property)
    {
        return FAkUGCCommandExecutionResult::Failure(TEXT("editor.propertyId"), TEXT("Property is not exposed by the prefab schema."));
    }
    if (Property->ValueType != Value.Type)
    {
        return FAkUGCCommandExecutionResult::Failure(TEXT("editor.propertyValue"), TEXT("Property value type does not match the prefab schema."));
    }

    const double NumericValue = Value.Type == EAkUGCValueType::Integer
        ? static_cast<double>(Value.IntegerValue)
        : Value.NumberValue;
    if ((Value.Type == EAkUGCValueType::Integer || Value.Type == EAkUGCValueType::Number)
        && ((Property->bHasMinimum && NumericValue < Property->Minimum)
            || (Property->bHasMaximum && NumericValue > Property->Maximum)))
    {
        return FAkUGCCommandExecutionResult::Failure(TEXT("editor.propertyValue"), TEXT("Property value is outside the allowed range."));
    }

    FAkUGCCommand Command;
    Command.CommandId = FGuid::NewGuid();
    Command.Type = EAkUGCCommandType::SetProperty;
    Command.SceneId = ActiveSceneId;
    Command.EntityId = EntityId;
    Command.ComponentTypeId = ComponentTypeId;
    Command.PropertyId = PropertyId;
    Command.PropertyValue = Value;

    FAkUGCCommandTransaction Transaction;
    Transaction.TransactionId = FGuid::NewGuid();
    Transaction.Label = FString::Printf(TEXT("Set %s.%s"), *ComponentTypeId.ToString(), *PropertyId.ToString());
    Transaction.Commands.Add(MoveTemp(Command));

    TGuardValue<bool> ApplyingGuard(bApplyingUGCTransaction, true);
    return Session->Execute(Document, Transaction);
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
    if (!Session)
    {
        return NoSessionResult();
    }
    TGuardValue<bool> ApplyingGuard(bApplyingUGCTransaction, true);
    return Session->Undo(Document);
}

FAkUGCCommandExecutionResult UAkUGCEditorSubsystem::Redo()
{
    if (!Session)
    {
        return NoSessionResult();
    }
    TGuardValue<bool> ApplyingGuard(bApplyingUGCTransaction, true);
    return Session->Redo(Document);
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
    return Session && Session->CanUndo();
}

bool UAkUGCEditorSubsystem::CanRedo() const
{
    return Session && Session->CanRedo();
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
        NewSceneId);

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

#include "Subsystem/AkUGCEditorSubsystem.h"

#include "Command/AkUGCCommand.h"
#include "Document/AkUGCDocumentJson.h"
#include "Editor.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
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
}

void UAkUGCEditorSubsystem::Deinitialize()
{
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
    return Session->Execute(Document, Transaction);
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
    return Session->Execute(Document, Transaction);
}

FAkUGCCommandExecutionResult UAkUGCEditorSubsystem::SetEntityTransform(
    const FGuid& EntityId,
    const FTransform& Transform)
{
    if (!Session)
    {
        return NoSessionResult();
    }

    FAkUGCCommand Command;
    Command.CommandId = FGuid::NewGuid();
    Command.Type = EAkUGCCommandType::SetTransform;
    Command.SceneId = ActiveSceneId;
    Command.EntityId = EntityId;
    Command.Transform = Transform;

    FAkUGCCommandTransaction Transaction;
    Transaction.TransactionId = FGuid::NewGuid();
    Transaction.Label = TEXT("Move entity");
    Transaction.Commands.Add(MoveTemp(Command));
    return Session->Execute(Document, Transaction);
}

FAkUGCCommandExecutionResult UAkUGCEditorSubsystem::Undo()
{
    return Session ? Session->Undo(Document) : NoSessionResult();
}

FAkUGCCommandExecutionResult UAkUGCEditorSubsystem::Redo()
{
    return Session ? Session->Redo(Document) : NoSessionResult();
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

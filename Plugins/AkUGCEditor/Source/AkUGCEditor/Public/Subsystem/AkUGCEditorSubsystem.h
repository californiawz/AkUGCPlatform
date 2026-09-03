#pragma once

#include "Command/AkUGCCommandExecutor.h"
#include "Document/AkUGCDocument.h"
#include "EditorSubsystem.h"
#include "Prefab/AkUGCPrefabRegistry.h"
#include "Scene/AkUGCSceneRuntime.h"
#include "Session/AkUGCDocumentRuntimeSession.h"
#include "AkUGCEditorSubsystem.generated.h"

class AActor;

UCLASS()
class AKUGCEDITOR_API UAkUGCEditorSubsystem : public UEditorSubsystem
{
    GENERATED_BODY()

public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    bool NewTowerDefenseProject(FString* OutError = nullptr);
    bool SaveProject(const FString& FilePath, FString* OutError = nullptr) const;
    bool LoadProject(const FString& FilePath, FString* OutError = nullptr);
    void CloseProject();

    FAkUGCCommandExecutionResult PlacePrefab(
        FName PrefabId,
        const FTransform& Transform,
        FGuid& OutEntityId);

    FAkUGCCommandExecutionResult DeleteEntity(const FGuid& EntityId);
    FAkUGCCommandExecutionResult DuplicateEntity(const FGuid& SourceEntityId, FGuid& OutEntityId);
    FAkUGCCommandExecutionResult SetEntityTransform(const FGuid& EntityId, const FTransform& Transform);
    FAkUGCCommandExecutionResult SetEntityTransforms(const TMap<FGuid, FTransform>& Transforms, const FString& Label);
    FAkUGCCommandExecutionResult SetEntityProperty(
        const FGuid& EntityId,
        FName ComponentTypeId,
        FName PropertyId,
        const FAkUGCValue& Value);
    FAkUGCCommandExecutionResult DeleteSelectedEntity();
    FAkUGCCommandExecutionResult DuplicateSelectedEntity(FGuid& OutEntityId);
    FAkUGCCommandExecutionResult Undo();
    FAkUGCCommandExecutionResult Redo();

    bool SelectEntity(const FGuid& EntityId);
    FGuid GetSelectedEntityId() const;
    const FAkUGCEntityRecord* FindEntity(const FGuid& EntityId) const;
    const FAkUGCPrefabDefinition* FindPrefabForEntity(const FGuid& EntityId) const;
    bool HasOpenProject() const;
    bool CanUndo() const;
    bool CanRedo() const;
    const FAkUGCProjectDocument& GetDocument() const;
    const FAkUGCPrefabRegistry& GetPrefabRegistry() const;
    FString GetDefaultProjectPath() const;

private:
    bool OpenDocument(FAkUGCProjectDocument&& NewDocument, FString* OutError);
    bool CreateSession(FString* OutError);
    FAkUGCCommandExecutionResult NoSessionResult() const;
    void RegisterEditorDelegates();
    void UnregisterEditorDelegates();
    void OnActorSelectionChanged(const TArray<UObject*>& NewSelection, bool bForceRefresh);
    void OnActorsMoved(TArray<AActor*>& Actors);
    void AddActorAndUGCDescendants(AActor* Actor, TMap<FGuid, FTransform>& OutTransforms) const;

    FAkUGCProjectDocument Document;
    TUniquePtr<FAkUGCPrefabRegistry> PrefabRegistry;
    TUniquePtr<FAkUGCSceneRuntime> Runtime;
    TUniquePtr<FAkUGCDocumentRuntimeSession> Session;
    FGuid ActiveSceneId;
    FGuid SelectedEntityId;
    FDelegateHandle SelectionChangedHandle;
    FDelegateHandle ActorsMovedHandle;
    bool bUpdatingEditorSelection = false;
    bool bApplyingUGCTransaction = false;
};

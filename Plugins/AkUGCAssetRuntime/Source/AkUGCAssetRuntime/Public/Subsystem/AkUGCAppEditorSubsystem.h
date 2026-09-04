#pragma once

#include "CoreMinimal.h"
#include "Document/AkUGCDocument.h"
#include "Prefab/AkUGCPrefabDefinition.h"
#include "Subsystems/WorldSubsystem.h"
#include "AkUGCAppEditorSubsystem.generated.h"

struct FAkUGCCommandExecutionResult;
class FAkUGCDocumentRuntimeSession;
class FAkUGCPrefabRegistry;
class FAkUGCRuntimeCommandService;
class FAkUGCSceneRuntime;

USTRUCT(BlueprintType)
struct AKUGCASSETRUNTIME_API FAkUGCAppEditResult
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "UGC")
    bool bSucceeded = false;

    UPROPERTY(BlueprintReadOnly, Category = "UGC")
    FString ErrorPath;

    UPROPERTY(BlueprintReadOnly, Category = "UGC")
    FString ErrorMessage;
};

UCLASS()
class AKUGCASSETRUNTIME_API UAkUGCAppEditorSubsystem : public UWorldSubsystem
{
    GENERATED_BODY()

public:
    virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    UFUNCTION(BlueprintCallable, Category = "UGC|App Editor")
    FAkUGCAppEditResult NewTowerDefenseProject();

    UFUNCTION(BlueprintCallable, Category = "UGC|App Editor")
    FAkUGCAppEditResult LoadProjectJson(const FString& Json);

    UFUNCTION(BlueprintCallable, Category = "UGC|App Editor")
    FAkUGCAppEditResult ExportProjectJson(FString& OutJson) const;

    UFUNCTION(BlueprintCallable, Category = "UGC|App Editor")
    void CloseProject();

    UFUNCTION(BlueprintCallable, Category = "UGC|App Editor")
    FAkUGCAppEditResult PlacePrefab(FName PrefabId, const FTransform& Transform, FGuid& OutEntityId);

    UFUNCTION(BlueprintCallable, Category = "UGC|App Editor")
    FAkUGCAppEditResult DeleteEntities(const TArray<FGuid>& EntityIds);

    UFUNCTION(BlueprintCallable, Category = "UGC|App Editor")
    FAkUGCAppEditResult DuplicateEntity(const FGuid& SourceEntityId, const FVector& WorldOffset, FGuid& OutEntityId);

    UFUNCTION(BlueprintCallable, Category = "UGC|App Editor")
    FAkUGCAppEditResult SetEntityTransform(const FGuid& EntityId, const FTransform& Transform);

    UFUNCTION(BlueprintCallable, Category = "UGC|App Editor")
    FAkUGCAppEditResult SetEntityParent(const FGuid& EntityId, const FGuid& ParentEntityId);

    UFUNCTION(BlueprintCallable, Category = "UGC|App Editor")
    FAkUGCAppEditResult SetEntityProperty(
        const FGuid& EntityId,
        FName ComponentTypeId,
        FName PropertyId,
        const FAkUGCValue& Value);

    UFUNCTION(BlueprintCallable, Category = "UGC|App Editor")
    FAkUGCAppEditResult Undo();

    UFUNCTION(BlueprintCallable, Category = "UGC|App Editor")
    FAkUGCAppEditResult Redo();

    UFUNCTION(BlueprintPure, Category = "UGC|App Editor")
    bool HasOpenProject() const;

    UFUNCTION(BlueprintPure, Category = "UGC|App Editor")
    bool CanUndo() const;

    UFUNCTION(BlueprintPure, Category = "UGC|App Editor")
    bool CanRedo() const;

    UFUNCTION(BlueprintPure, Category = "UGC|App Editor")
    int64 GetDocumentRevision() const;

    UFUNCTION(BlueprintPure, Category = "UGC|App Editor")
    TArray<FName> GetAvailablePrefabIds() const;

    UFUNCTION(BlueprintPure, Category = "UGC|App Editor")
    bool GetEntityRecord(const FGuid& EntityId, FAkUGCEntityRecord& OutEntity) const;

    UFUNCTION(BlueprintPure, Category = "UGC|App Editor")
    bool GetEditableProperties(const FGuid& EntityId, TArray<FAkUGCPropertyDefinition>& OutProperties) const;

    const FAkUGCProjectDocument& GetDocument() const;

protected:
    virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
    bool OpenDocument(FAkUGCProjectDocument&& NewDocument, FString& OutError);
    bool ValidateMobileDocument(const FAkUGCProjectDocument& Candidate, FString& OutError) const;
    bool ValidatePlacement(FName PrefabId, const FTransform& Transform, FString& OutError) const;
    bool ValidateDocumentBudgets(const FAkUGCProjectDocument& Candidate, FString& OutError) const;
    FAkUGCAppEditResult ExecuteResult(const FAkUGCCommandExecutionResult& Result);
    static FAkUGCAppEditResult Success();
    static FAkUGCAppEditResult Failure(FString Path, FString Message);

    FAkUGCProjectDocument Document;
    TUniquePtr<FAkUGCPrefabRegistry> PrefabRegistry;
    TUniquePtr<FAkUGCSceneRuntime> Runtime;
    TUniquePtr<FAkUGCDocumentRuntimeSession> Session;
    TUniquePtr<FAkUGCRuntimeCommandService> CommandService;
    FGuid ActiveSceneId;
    uint64 DocumentRevision = 0;
};

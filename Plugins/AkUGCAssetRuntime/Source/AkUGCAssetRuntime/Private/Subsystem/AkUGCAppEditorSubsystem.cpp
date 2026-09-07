#include "Subsystem/AkUGCAppEditorSubsystem.h"

#include "Command/AkUGCRuntimeCommandService.h"
#include "Document/AkUGCDocumentJson.h"
#include "Engine/World.h"
#include "Prefab/AkUGCOfficialPrefabCatalog.h"
#include "Prefab/AkUGCPrefabRegistry.h"
#include "Scene/AkUGCSceneRuntime.h"
#include "Session/AkUGCDocumentRuntimeSession.h"
#include "Validation/AkUGCDocumentValidator.h"

namespace
{
    constexpr int32 MaxMobileEntityUnits = 500;
    constexpr int32 MaxMobileRenderUnits = 1000;
    constexpr int32 MaxMobilePhysicsUnits = 500;
    constexpr int32 MaxMobileScriptUnits = 500;

    bool IsAppIntegerWithinRange(int64 Value, const FAkUGCPropertyDefinition& Property)
    {
        constexpr double Int64ExclusiveUpper = 9223372036854775808.0;
        constexpr double Int64Lower = -9223372036854775808.0;
        if (Property.bHasMinimum && Property.Minimum >= Int64ExclusiveUpper)
        {
            return false;
        }
        if (Property.bHasMaximum && Property.Maximum < Int64Lower)
        {
            return false;
        }
        if (Property.bHasMinimum && Property.Minimum > Int64Lower
            && Value < static_cast<int64>(FMath::CeilToDouble(Property.Minimum)))
        {
            return false;
        }
        if (Property.bHasMaximum && Property.Maximum < Int64ExclusiveUpper
            && Value > static_cast<int64>(FMath::FloorToDouble(Property.Maximum)))
        {
            return false;
        }
        return true;
    }

    bool IsPropertyValueValid(const FAkUGCValue& Value, const FAkUGCPropertyDefinition& Property)
    {
        if (Value.Type != Property.ValueType)
        {
            return false;
        }
        if (Value.Type == EAkUGCValueType::Number)
        {
            return FMath::IsFinite(Value.NumberValue)
                && (!Property.bHasMinimum || Value.NumberValue >= Property.Minimum)
                && (!Property.bHasMaximum || Value.NumberValue <= Property.Maximum);
        }
        if (Value.Type == EAkUGCValueType::Integer)
        {
            return IsAppIntegerWithinRange(Value.IntegerValue, Property);
        }
        if (Value.Type == EAkUGCValueType::Vector)
        {
            return !Value.VectorValue.ContainsNaN();
        }
        if (Value.Type == EAkUGCValueType::Rotator)
        {
            return !Value.RotatorValue.ContainsNaN();
        }
        return true;
    }
}

bool UAkUGCAppEditorSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
#if UE_SERVER
    return false;
#else
    const UWorld* World = Cast<UWorld>(Outer);
    return Super::ShouldCreateSubsystem(Outer)
        && World
        && !World->IsNetMode(NM_DedicatedServer);
#endif
}

void UAkUGCAppEditorSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    PrefabRegistry = MakeUnique<FAkUGCPrefabRegistry>();

    FString Error;
    if (!FAkUGCOfficialPrefabCatalog::RegisterTowerDefense(*PrefabRegistry, &Error))
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to register App Editor prefab catalog: %s"), *Error);
    }
}

void UAkUGCAppEditorSubsystem::Deinitialize()
{
    CloseProject();
    PrefabRegistry.Reset();
    Super::Deinitialize();
}

FAkUGCAppEditResult UAkUGCAppEditorSubsystem::NewTowerDefenseProject()
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

    FString Error;
    return OpenDocument(MoveTemp(NewDocument), Error)
        ? Success()
        : Failure(TEXT("appEditor.project"), MoveTemp(Error));
}

FAkUGCAppEditResult UAkUGCAppEditorSubsystem::LoadProjectJson(const FString& Json)
{
    FAkUGCProjectDocument NewDocument;
    FString Error;
    if (!FAkUGCDocumentJson::Deserialize(Json, NewDocument, &Error))
    {
        return Failure(TEXT("appEditor.json"), MoveTemp(Error));
    }
    return OpenDocument(MoveTemp(NewDocument), Error)
        ? Success()
        : Failure(TEXT("appEditor.project"), MoveTemp(Error));
}

FAkUGCAppEditResult UAkUGCAppEditorSubsystem::ExportProjectJson(FString& OutJson) const
{
    OutJson.Reset();
    if (!HasOpenProject())
    {
        return Failure(TEXT("appEditor.session"), TEXT("No UGC project is open."));
    }

    FString Error;
    return FAkUGCDocumentJson::Serialize(Document, OutJson, &Error)
        ? Success()
        : Failure(TEXT("appEditor.json"), MoveTemp(Error));
}

void UAkUGCAppEditorSubsystem::CloseProject()
{
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

FAkUGCAppEditResult UAkUGCAppEditorSubsystem::PlacePrefab(
    FName PrefabId,
    const FTransform& Transform,
    FGuid& OutEntityId)
{
    OutEntityId.Invalidate();
    if (!CommandService)
    {
        return Failure(TEXT("appEditor.session"), TEXT("No UGC project is open."));
    }

    FString Error;
    if (!ValidatePlacement(PrefabId, Transform, Error))
    {
        return Failure(TEXT("appEditor.transform"), MoveTemp(Error));
    }

    FAkUGCProjectDocument Candidate = Document;
    FAkUGCEntityRecord NewEntity;
    if (!PrefabRegistry->CreateEntityRecord(PrefabId, FGuid::NewGuid(), Transform, NewEntity, &Error))
    {
        return Failure(TEXT("appEditor.prefabId"), MoveTemp(Error));
    }
    Candidate.Scenes[0].Entities.Add(MoveTemp(NewEntity));
    if (!ValidateDocumentBudgets(Candidate, Error))
    {
        return Failure(TEXT("appEditor.budget"), MoveTemp(Error));
    }
    return ExecuteResult(CommandService->PlacePrefab(PrefabId, Transform, OutEntityId));
}

FAkUGCAppEditResult UAkUGCAppEditorSubsystem::DeleteEntities(const TArray<FGuid>& EntityIds)
{
    if (!CommandService)
    {
        return Failure(TEXT("appEditor.session"), TEXT("No UGC project is open."));
    }

    TSet<FGuid> UniqueEntityIds;
    UniqueEntityIds.Append(EntityIds);
    return ExecuteResult(CommandService->DeleteEntities(UniqueEntityIds, TEXT("Delete entities")));
}

FAkUGCAppEditResult UAkUGCAppEditorSubsystem::DuplicateEntity(
    const FGuid& SourceEntityId,
    const FVector& WorldOffset,
    FGuid& OutEntityId)
{
    OutEntityId.Invalidate();
    if (!CommandService)
    {
        return Failure(TEXT("appEditor.session"), TEXT("No UGC project is open."));
    }

    const FAkUGCEntityRecord* Source = CommandService->FindEntity(SourceEntityId);
    if (!Source)
    {
        return Failure(TEXT("appEditor.sourceEntityId"), TEXT("Source entity does not exist."));
    }

    FTransform DuplicateTransform = Source->Transform;
    DuplicateTransform.AddToTranslation(WorldOffset);
    FString Error;
    if (!ValidatePlacement(Source->PrefabId, DuplicateTransform, Error))
    {
        return Failure(TEXT("appEditor.transform"), MoveTemp(Error));
    }

    FAkUGCProjectDocument Candidate = Document;
    FAkUGCEntityRecord Duplicate = *Source;
    Duplicate.EntityId = FGuid::NewGuid();
    Duplicate.Transform = DuplicateTransform;
    Candidate.Scenes[0].Entities.Add(MoveTemp(Duplicate));
    if (!ValidateDocumentBudgets(Candidate, Error))
    {
        return Failure(TEXT("appEditor.budget"), MoveTemp(Error));
    }
    return ExecuteResult(CommandService->DuplicateEntity(SourceEntityId, WorldOffset, OutEntityId));
}

FAkUGCAppEditResult UAkUGCAppEditorSubsystem::SetEntityTransform(
    const FGuid& EntityId,
    const FTransform& Transform)
{
    if (!CommandService)
    {
        return Failure(TEXT("appEditor.session"), TEXT("No UGC project is open."));
    }
    const FAkUGCEntityRecord* Entity = CommandService->FindEntity(EntityId);
    if (!Entity)
    {
        return Failure(TEXT("appEditor.entityId"), TEXT("Entity does not exist."));
    }
    FString Error;
    if (!ValidatePlacement(Entity->PrefabId, Transform, Error))
    {
        return Failure(TEXT("appEditor.transform"), MoveTemp(Error));
    }
    return ExecuteResult(CommandService->SetEntityTransform(EntityId, Transform));
}

FAkUGCAppEditResult UAkUGCAppEditorSubsystem::SetEntityParent(
    const FGuid& EntityId,
    const FGuid& ParentEntityId)
{
    return CommandService
        ? ExecuteResult(CommandService->SetEntityParent(EntityId, ParentEntityId))
        : Failure(TEXT("appEditor.session"), TEXT("No UGC project is open."));
}

FAkUGCAppEditResult UAkUGCAppEditorSubsystem::SetEntityProperty(
    const FGuid& EntityId,
    FName ComponentTypeId,
    FName PropertyId,
    const FAkUGCValue& Value)
{
    return CommandService
        ? ExecuteResult(CommandService->SetEntityProperty(EntityId, ComponentTypeId, PropertyId, Value))
        : Failure(TEXT("appEditor.session"), TEXT("No UGC project is open."));
}

FAkUGCAppEditResult UAkUGCAppEditorSubsystem::Undo()
{
    return CommandService
        ? ExecuteResult(CommandService->Undo())
        : Failure(TEXT("appEditor.session"), TEXT("No UGC project is open."));
}

FAkUGCAppEditResult UAkUGCAppEditorSubsystem::Redo()
{
    return CommandService
        ? ExecuteResult(CommandService->Redo())
        : Failure(TEXT("appEditor.session"), TEXT("No UGC project is open."));
}

bool UAkUGCAppEditorSubsystem::HasOpenProject() const
{
    return CommandService.IsValid() && ActiveSceneId.IsValid();
}

bool UAkUGCAppEditorSubsystem::CanUndo() const
{
    return CommandService && CommandService->CanUndo();
}

bool UAkUGCAppEditorSubsystem::CanRedo() const
{
    return CommandService && CommandService->CanRedo();
}

int64 UAkUGCAppEditorSubsystem::GetDocumentRevision() const
{
    return static_cast<int64>(DocumentRevision);
}

TArray<FName> UAkUGCAppEditorSubsystem::GetAvailablePrefabIds() const
{
    return PrefabRegistry ? PrefabRegistry->GetRegisteredIds() : TArray<FName>{};
}

bool UAkUGCAppEditorSubsystem::GetEntityRecord(
    const FGuid& EntityId,
    FAkUGCEntityRecord& OutEntity) const
{
    OutEntity = FAkUGCEntityRecord{};
    if (!CommandService)
    {
        return false;
    }
    const FAkUGCEntityRecord* Entity = CommandService->FindEntity(EntityId);
    if (!Entity)
    {
        return false;
    }
    OutEntity = *Entity;
    return true;
}

bool UAkUGCAppEditorSubsystem::GetEditableProperties(
    const FGuid& EntityId,
    TArray<FAkUGCPropertyDefinition>& OutProperties) const
{
    OutProperties.Reset();
    if (!CommandService || !PrefabRegistry)
    {
        return false;
    }
    const FAkUGCEntityRecord* Entity = CommandService->FindEntity(EntityId);
    const FAkUGCPrefabDefinition* Prefab = Entity ? PrefabRegistry->Find(Entity->PrefabId) : nullptr;
    if (!Prefab)
    {
        return false;
    }
    OutProperties = Prefab->EditableProperties.FilterByPredicate([](const FAkUGCPropertyDefinition& Property)
    {
        return Property.bMobileEditable;
    });
    return true;
}

const FAkUGCProjectDocument& UAkUGCAppEditorSubsystem::GetDocument() const
{
    return Document;
}

bool UAkUGCAppEditorSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
#if UE_SERVER
    return false;
#else
    return WorldType == EWorldType::Game
        || WorldType == EWorldType::PIE
        || WorldType == EWorldType::GamePreview;
#endif
}

bool UAkUGCAppEditorSubsystem::ValidateMobileDocument(
    const FAkUGCProjectDocument& Candidate,
    FString& OutError) const
{
    if (!PrefabRegistry)
    {
        OutError = TEXT("Prefab registry is not initialized.");
        return false;
    }

    for (const FAkUGCSceneDocument& Scene : Candidate.Scenes)
    {
        for (const FAkUGCEntityRecord& Entity : Scene.Entities)
        {
            const FAkUGCPrefabDefinition* Prefab = PrefabRegistry->Find(Entity.PrefabId);
            if (!Prefab)
            {
                OutError = FString::Printf(TEXT("Prefab '%s' is not registered."), *Entity.PrefabId.ToString());
                return false;
            }
            if (!ValidatePlacement(Entity.PrefabId, Entity.Transform, OutError))
            {
                return false;
            }

            TSet<FName> ComponentTypeIds;
            for (const FAkUGCComponentRecord& Component : Entity.Components)
            {
                if (ComponentTypeIds.Contains(Component.TypeId))
                {
                    OutError = FString::Printf(
                        TEXT("Component '%s' is duplicated on entity '%s'."),
                        *Component.TypeId.ToString(),
                        *Entity.EntityId.ToString());
                    return false;
                }
                ComponentTypeIds.Add(Component.TypeId);

                const FAkUGCComponentRecord* DefaultComponent = Prefab->DefaultComponents.FindByPredicate(
                    [&Component](const FAkUGCComponentRecord& CandidateComponent)
                    {
                        return CandidateComponent.TypeId == Component.TypeId;
                    });
                if (!DefaultComponent)
                {
                    OutError = FString::Printf(
                        TEXT("Component '%s' is not declared by prefab '%s'."),
                        *Component.TypeId.ToString(),
                        *Entity.PrefabId.ToString());
                    return false;
                }
                if (Component.SchemaVersion != DefaultComponent->SchemaVersion)
                {
                    OutError = FString::Printf(
                        TEXT("Component '%s' schema version %d does not match prefab version %d."),
                        *Component.TypeId.ToString(),
                        Component.SchemaVersion,
                        DefaultComponent->SchemaVersion);
                    return false;
                }

                for (const TPair<FName, FAkUGCValue>& PropertyPair : Component.Properties)
                {
                    const FAkUGCPropertyDefinition* Property = Prefab->EditableProperties.FindByPredicate(
                        [&Component, &PropertyPair](const FAkUGCPropertyDefinition& CandidateProperty)
                        {
                            return CandidateProperty.ComponentTypeId == Component.TypeId
                                && CandidateProperty.PropertyId == PropertyPair.Key;
                        });
                    if (!Property)
                    {
                        OutError = FString::Printf(
                            TEXT("Property '%s.%s' is not declared by prefab '%s'."),
                            *Component.TypeId.ToString(),
                            *PropertyPair.Key.ToString(),
                            *Entity.PrefabId.ToString());
                        return false;
                    }
                    const auto ValuesEqual = [](const FAkUGCValue& Left, const FAkUGCValue& Right)
                    {
                        if (Left.Type != Right.Type)
                        {
                            return false;
                        }
                        switch (Left.Type)
                        {
                        case EAkUGCValueType::Bool: return Left.BoolValue == Right.BoolValue;
                        case EAkUGCValueType::Integer: return Left.IntegerValue == Right.IntegerValue;
                        case EAkUGCValueType::Number: return Left.NumberValue == Right.NumberValue;
                        case EAkUGCValueType::String: return Left.StringValue == Right.StringValue;
                        case EAkUGCValueType::Name: return Left.NameValue == Right.NameValue;
                        case EAkUGCValueType::Vector: return Left.VectorValue.Equals(Right.VectorValue);
                        case EAkUGCValueType::Rotator: return Left.RotatorValue.Equals(Right.RotatorValue);
                        }
                        return false;
                    };
                    if (!Property->bMobileEditable && !ValuesEqual(PropertyPair.Value, Property->DefaultValue))
                    {
                        OutError = FString::Printf(
                            TEXT("Property '%s.%s' is not editable on mobile and must keep its default value."),
                            *Component.TypeId.ToString(),
                            *PropertyPair.Key.ToString());
                        return false;
                    }
                    if (!IsPropertyValueValid(PropertyPair.Value, *Property))
                    {
                        OutError = FString::Printf(
                            TEXT("Property '%s.%s' value violates its schema."),
                            *Component.TypeId.ToString(),
                            *PropertyPair.Key.ToString());
                        return false;
                    }
                }
            }

            for (const FAkUGCComponentRecord& RequiredComponent : Prefab->DefaultComponents)
            {
                if (!ComponentTypeIds.Contains(RequiredComponent.TypeId))
                {
                    OutError = FString::Printf(
                        TEXT("Required component '%s' is missing from prefab '%s'."),
                        *RequiredComponent.TypeId.ToString(),
                        *Entity.PrefabId.ToString());
                    return false;
                }
            }
            for (const FAkUGCPropertyDefinition& RequiredProperty : Prefab->EditableProperties)
            {
                const FAkUGCComponentRecord* Component = Entity.Components.FindByPredicate(
                    [&RequiredProperty](const FAkUGCComponentRecord& CandidateComponent)
                    {
                        return CandidateComponent.TypeId == RequiredProperty.ComponentTypeId;
                    });
                if (!Component || !Component->Properties.Contains(RequiredProperty.PropertyId))
                {
                    OutError = FString::Printf(
                        TEXT("Required property '%s.%s' is missing from prefab '%s'."),
                        *RequiredProperty.ComponentTypeId.ToString(),
                        *RequiredProperty.PropertyId.ToString(),
                        *Entity.PrefabId.ToString());
                    return false;
                }
            }
        }
    }
    return true;
}

bool UAkUGCAppEditorSubsystem::ValidatePlacement(
    FName PrefabId,
    const FTransform& Transform,
    FString& OutError) const
{
    const FAkUGCPrefabDefinition* Prefab = PrefabRegistry ? PrefabRegistry->Find(PrefabId) : nullptr;
    if (!Prefab)
    {
        OutError = FString::Printf(TEXT("Prefab '%s' is not registered."), *PrefabId.ToString());
        return false;
    }
    if (Transform.ContainsNaN())
    {
        OutError = TEXT("Transform must be finite.");
        return false;
    }

    const FVector Scale = Transform.GetScale3D();
    if (!Prefab->Placement.bAllowScale && !Scale.Equals(FVector::OneVector, KINDA_SMALL_NUMBER))
    {
        OutError = TEXT("Prefab does not allow scaling.");
        return false;
    }
    if ((Scale - Prefab->Placement.MinimumScale).GetMin() < 0.0
        || (Prefab->Placement.MaximumScale - Scale).GetMin() < 0.0)
    {
        OutError = TEXT("Prefab scale is outside its allowed range.");
        return false;
    }
    return true;
}

bool UAkUGCAppEditorSubsystem::ValidateDocumentBudgets(
    const FAkUGCProjectDocument& Candidate,
    FString& OutError) const
{
    int64 EntityUnits = 0;
    int64 RenderUnits = 0;
    int64 PhysicsUnits = 0;
    int64 ScriptUnits = 0;
    for (const FAkUGCSceneDocument& Scene : Candidate.Scenes)
    {
        for (const FAkUGCEntityRecord& Entity : Scene.Entities)
        {
            const FAkUGCPrefabDefinition* Prefab = PrefabRegistry ? PrefabRegistry->Find(Entity.PrefabId) : nullptr;
            if (!Prefab)
            {
                OutError = FString::Printf(TEXT("Prefab '%s' is not registered."), *Entity.PrefabId.ToString());
                return false;
            }
            EntityUnits += Prefab->Cost.EntityUnits;
            RenderUnits += Prefab->Cost.RenderUnits;
            PhysicsUnits += Prefab->Cost.PhysicsUnits;
            ScriptUnits += Prefab->Cost.ScriptUnits;
        }
    }

    if (EntityUnits > MaxMobileEntityUnits
        || RenderUnits > MaxMobileRenderUnits
        || PhysicsUnits > MaxMobilePhysicsUnits
        || ScriptUnits > MaxMobileScriptUnits)
    {
        OutError = FString::Printf(
            TEXT("Mobile content budget exceeded: entity=%lld/%d render=%lld/%d physics=%lld/%d script=%lld/%d."),
            EntityUnits,
            MaxMobileEntityUnits,
            RenderUnits,
            MaxMobileRenderUnits,
            PhysicsUnits,
            MaxMobilePhysicsUnits,
            ScriptUnits,
            MaxMobileScriptUnits);
        return false;
    }
    return true;
}

bool UAkUGCAppEditorSubsystem::OpenDocument(
    FAkUGCProjectDocument&& NewDocument,
    FString& OutError)
{
    const FAkUGCValidationResult Validation = FAkUGCDocumentValidator::Validate(NewDocument);
    if (!Validation.IsValid())
    {
        const FAkUGCValidationIssue* FirstError = Validation.Issues.FindByPredicate([](const FAkUGCValidationIssue& Issue)
        {
            return Issue.Severity == EAkUGCValidationSeverity::Error;
        });
        OutError = FirstError
            ? FString::Printf(TEXT("%s: %s"), *FirstError->Path, *FirstError->Message)
            : TEXT("UGC project validation failed.");
        return false;
    }
    if (NewDocument.Scenes.IsEmpty())
    {
        OutError = TEXT("UGC project must contain at least one scene.");
        return false;
    }
    if (!PrefabRegistry)
    {
        OutError = TEXT("Prefab registry is not initialized.");
        return false;
    }
    if (!ValidateMobileDocument(NewDocument, OutError)
        || !ValidateDocumentBudgets(NewDocument, OutError))
    {
        return false;
    }

    UWorld* World = GetWorld();
    if (!World)
    {
        OutError = TEXT("Runtime world is not available.");
        return false;
    }

    const bool bHadOpenProject = HasOpenProject();
    const FGuid PreviousSceneId = ActiveSceneId;
    CommandService.Reset();
    Session.Reset();
    if (Runtime)
    {
        Runtime->Unload();
    }
    Runtime.Reset();
    ActiveSceneId.Invalidate();

    const FGuid NewSceneId = NewDocument.Scenes[0].SceneId;
    TUniquePtr<FAkUGCSceneRuntime> NewRuntime = MakeUnique<FAkUGCSceneRuntime>(World);
    TUniquePtr<FAkUGCDocumentRuntimeSession> NewSession = MakeUnique<FAkUGCDocumentRuntimeSession>(
        *NewRuntime,
        *PrefabRegistry,
        NewSceneId,
        EAkUGCRuntimeSessionMode::Edit);
    const FAkUGCCommandExecutionResult Result = NewSession->Initialize(NewDocument);
    if (!Result.bSucceeded)
    {
        OutError = Result.ErrorMessage;
        if (bHadOpenProject)
        {
            Runtime = MakeUnique<FAkUGCSceneRuntime>(World);
            Session = MakeUnique<FAkUGCDocumentRuntimeSession>(
                *Runtime,
                *PrefabRegistry,
                PreviousSceneId,
                EAkUGCRuntimeSessionMode::Edit);
            const FAkUGCCommandExecutionResult RestoreResult = Session->Initialize(Document);
            if (RestoreResult.bSucceeded)
            {
                ActiveSceneId = PreviousSceneId;
                CommandService = MakeUnique<FAkUGCRuntimeCommandService>(
                    Document,
                    *Session,
                    *PrefabRegistry,
                    ActiveSceneId,
                    EAkUGCEditingClient::Mobile);
            }
            else
            {
                OutError += FString::Printf(TEXT("; previous project restore failed: %s"), *RestoreResult.ErrorMessage);
                CommandService.Reset();
                Session.Reset();
                Runtime.Reset();
            }
        }
        return false;
    }

    Document = MoveTemp(NewDocument);
    ActiveSceneId = NewSceneId;
    Runtime = MoveTemp(NewRuntime);
    Session = MoveTemp(NewSession);
    CommandService = MakeUnique<FAkUGCRuntimeCommandService>(
        Document,
        *Session,
        *PrefabRegistry,
        ActiveSceneId,
        EAkUGCEditingClient::Mobile);
    ++DocumentRevision;
    return true;
}

FAkUGCAppEditResult UAkUGCAppEditorSubsystem::ExecuteResult(
    const FAkUGCCommandExecutionResult& Result)
{
    if (Result.bSucceeded)
    {
        ++DocumentRevision;
        return Success();
    }
    return Failure(Result.ErrorPath, Result.ErrorMessage);
}

FAkUGCAppEditResult UAkUGCAppEditorSubsystem::Success()
{
    FAkUGCAppEditResult Result;
    Result.bSucceeded = true;
    return Result;
}

FAkUGCAppEditResult UAkUGCAppEditorSubsystem::Failure(FString Path, FString Message)
{
    FAkUGCAppEditResult Result;
    Result.ErrorPath = MoveTemp(Path);
    Result.ErrorMessage = MoveTemp(Message);
    return Result;
}

#include "Validation/AkUGCDocumentValidator.h"

#include "Document/AkUGCDocument.h"

bool FAkUGCValidationResult::IsValid() const
{
    return !Issues.ContainsByPredicate([](const FAkUGCValidationIssue& Issue)
    {
        return Issue.Severity == EAkUGCValidationSeverity::Error;
    });
}

void FAkUGCValidationResult::AddError(FString Path, FString Message)
{
    Issues.Add({EAkUGCValidationSeverity::Error, MoveTemp(Path), MoveTemp(Message)});
}

void FAkUGCValidationResult::AddWarning(FString Path, FString Message)
{
    Issues.Add({EAkUGCValidationSeverity::Warning, MoveTemp(Path), MoveTemp(Message)});
}

FAkUGCValidationResult FAkUGCDocumentValidator::Validate(const FAkUGCProjectDocument& Document)
{
    FAkUGCValidationResult Result;

    if (Document.Manifest.SchemaVersion != AkUGCSchema::CurrentProjectDocumentVersion)
    {
        Result.AddError(
            TEXT("manifest.schemaVersion"),
            FString::Printf(
                TEXT("Unsupported schema version %d; expected %d."),
                Document.Manifest.SchemaVersion,
                AkUGCSchema::CurrentProjectDocumentVersion));
    }

    if (!Document.Manifest.ProjectId.IsValid())
    {
        Result.AddError(TEXT("manifest.projectId"), TEXT("Project ID must be a valid GUID."));
    }

    if (Document.Manifest.DisplayName.TrimStartAndEnd().IsEmpty())
    {
        Result.AddError(TEXT("manifest.displayName"), TEXT("Project display name is required."));
    }

    if (Document.Manifest.TemplateId.IsNone())
    {
        Result.AddError(TEXT("manifest.templateId"), TEXT("Template ID is required."));
    }

    TSet<FGuid> SceneIds;
    TSet<FGuid> EntityIds;

    for (int32 SceneIndex = 0; SceneIndex < Document.Scenes.Num(); ++SceneIndex)
    {
        const FAkUGCSceneDocument& Scene = Document.Scenes[SceneIndex];
        const FString ScenePath = FString::Printf(TEXT("scenes[%d]"), SceneIndex);

        if (!Scene.SceneId.IsValid())
        {
            Result.AddError(ScenePath + TEXT(".sceneId"), TEXT("Scene ID must be a valid GUID."));
        }
        else if (SceneIds.Contains(Scene.SceneId))
        {
            Result.AddError(ScenePath + TEXT(".sceneId"), TEXT("Scene ID must be unique."));
        }
        else
        {
            SceneIds.Add(Scene.SceneId);
        }

        for (int32 EntityIndex = 0; EntityIndex < Scene.Entities.Num(); ++EntityIndex)
        {
            const FAkUGCEntityRecord& Entity = Scene.Entities[EntityIndex];
            const FString EntityPath = FString::Printf(TEXT("%s.entities[%d]"), *ScenePath, EntityIndex);

            if (!Entity.EntityId.IsValid())
            {
                Result.AddError(EntityPath + TEXT(".entityId"), TEXT("Entity ID must be a valid GUID."));
            }
            else if (EntityIds.Contains(Entity.EntityId))
            {
                Result.AddError(EntityPath + TEXT(".entityId"), TEXT("Entity ID must be globally unique in the project."));
            }
            else
            {
                EntityIds.Add(Entity.EntityId);
            }

            if (Entity.PrefabId.IsNone())
            {
                Result.AddError(EntityPath + TEXT(".prefabId"), TEXT("Prefab ID is required."));
            }

            for (int32 ComponentIndex = 0; ComponentIndex < Entity.Components.Num(); ++ComponentIndex)
            {
                const FAkUGCComponentRecord& Component = Entity.Components[ComponentIndex];
                const FString ComponentPath = FString::Printf(
                    TEXT("%s.components[%d]"),
                    *EntityPath,
                    ComponentIndex);

                if (Component.TypeId.IsNone())
                {
                    Result.AddError(ComponentPath + TEXT(".typeId"), TEXT("Component type ID is required."));
                }
                if (Component.SchemaVersion < 1)
                {
                    Result.AddError(ComponentPath + TEXT(".schemaVersion"), TEXT("Component schema version must be positive."));
                }
            }
        }
    }

    for (int32 SceneIndex = 0; SceneIndex < Document.Scenes.Num(); ++SceneIndex)
    {
        const FAkUGCSceneDocument& Scene = Document.Scenes[SceneIndex];
        for (int32 EntityIndex = 0; EntityIndex < Scene.Entities.Num(); ++EntityIndex)
        {
            const FAkUGCEntityRecord& Entity = Scene.Entities[EntityIndex];
            if (Entity.ParentEntityId.IsValid() && !EntityIds.Contains(Entity.ParentEntityId))
            {
                Result.AddError(
                    FString::Printf(TEXT("scenes[%d].entities[%d].parentEntityId"), SceneIndex, EntityIndex),
                    TEXT("Parent entity does not exist in the project."));
            }
        }
    }

    if (Document.Scenes.IsEmpty())
    {
        Result.AddWarning(TEXT("scenes"), TEXT("Project does not contain a scene."));
    }

    return Result;
}

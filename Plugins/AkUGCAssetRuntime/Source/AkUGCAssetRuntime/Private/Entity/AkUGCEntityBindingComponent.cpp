#include "Entity/AkUGCEntityBindingComponent.h"

#include "GameFramework/Actor.h"

UAkUGCEntityBindingComponent::UAkUGCEntityBindingComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
}

void UAkUGCEntityBindingComponent::ApplyRecord(const FAkUGCEntityRecord& InRecord)
{
    EntityId = InRecord.EntityId;
    PrefabId = InRecord.PrefabId;
    SourceRecord = InRecord;

    if (AActor* Owner = GetOwner())
    {
        Owner->SetActorTransform(InRecord.Transform);
        Owner->Tags = InRecord.Tags;
    }
}

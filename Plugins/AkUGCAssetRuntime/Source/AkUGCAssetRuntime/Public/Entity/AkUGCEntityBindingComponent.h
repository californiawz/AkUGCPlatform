#pragma once

#include "Components/ActorComponent.h"
#include "Document/AkUGCDocument.h"
#include "AkUGCEntityBindingComponent.generated.h"

UCLASS(ClassGroup = "UGC", BlueprintType, Blueprintable, meta = (BlueprintSpawnableComponent))
class AKUGCASSETRUNTIME_API UAkUGCEntityBindingComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UAkUGCEntityBindingComponent();

    void ApplyRecord(const FAkUGCEntityRecord& InRecord);

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "UGC")
    FGuid EntityId;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "UGC")
    FName PrefabId;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "UGC")
    FAkUGCEntityRecord SourceRecord;
};

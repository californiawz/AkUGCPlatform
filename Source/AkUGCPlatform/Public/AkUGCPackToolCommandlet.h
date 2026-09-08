#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "AkUGCPackToolCommandlet.generated.h"

/** 生成签名 Logic Pack 文件与可信公钥，供 Dedicated Server 自动加载与 Headless 三波塔防验证。 */
UCLASS()
class AKUGCPLATFORM_API UAkUGCPackToolCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:
    virtual int32 Main(const FString& Params) override;
};

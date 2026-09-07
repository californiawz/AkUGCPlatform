#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "AkUGCGameMode.generated.h"

/**
 * 塔防权威会话的托管点。
 *
 * 服务器端负责创建 PlayAuthority 运行会话并投影权威状态到 GameState，
 * 客户端仅观察 GameState 的复制状态，不执行权威玩法。
 */
UCLASS()
class AKUGCPLATFORM_API AAkUGCGameMode : public AGameModeBase
{
    GENERATED_BODY()

public:
    AAkUGCGameMode();
};

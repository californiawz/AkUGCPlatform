#pragma once

#include "CoreMinimal.h"
#include "Document/AkUGCDocument.h"

namespace AkUGCPlayableSceneFactory
{
    /** 构造可通过服务器权威运行时校验的完整塔防场景（enemy_spawn + 路径 + 基地 + 终点 + 三波）。 */
    bool MakePlayableTowerDefenseDocument(FAkUGCProjectDocument& OutDocument, FString* OutError);
}

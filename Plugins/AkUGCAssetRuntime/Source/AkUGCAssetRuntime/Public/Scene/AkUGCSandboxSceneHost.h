#pragma once

#include "CoreMinimal.h"
#include "IAkUGCSandboxHost.h"

class FAkUGCSceneRuntime;

/**
 * FAkUGCSandboxSceneHost
 *
 * 把 Lua 沙箱受控 API 桥接到场景运行时（FAkUGCSceneRuntime）的真实宿主。
 *
 * 脚本通过 `ugc.get_health` / `ugc.apply_damage` 等受控能力间接访问场景实体的
 * 血量查询与伤害结算；实体 ID 使用 FGuid 的字符串形式在脚本与运行时之间传递。
 * 沙箱本身保持游戏无关，本类属于 AkUGCAssetRuntime 的游戏相关实现。
 */
class AKUGCASSETRUNTIME_API FAkUGCSandboxSceneHost : public IAkUGCSandboxHost
{
public:
	explicit FAkUGCSandboxSceneHost(FAkUGCSceneRuntime& InSceneRuntime);

	virtual void EmitMessage(const FString& Message) override;
	virtual bool QueryEntityHealth(const FString& EntityId, double& OutCurrent, double& OutMaximum) override;
	virtual bool ApplyDamage(
		const FString& SourceEntityId,
		const FString& TargetEntityId,
		double Damage,
		double& OutAppliedDamage,
		double& OutHealthAfter,
		bool& OutKilled,
		FString& OutError) override;

	/** 脚本调用 ugc.message 时累积的消息。 */
	const TArray<FString>& GetMessages() const { return Messages; }

private:
	FAkUGCSceneRuntime& SceneRuntime;
	TArray<FString> Messages;
};

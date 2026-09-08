// Copyright Akatsuki. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * FAkUGCSandboxWaveState
 *
 * 沙箱受控 Ruleset 查询返回的波次/胜负快照。沙箱层保持游戏无关：
 * 仅用基本数值表达状态，具体枚举语义由宿主（如塔防运行时）负责映射。
 */
struct AKUGCSANDBOX_API FAkUGCSandboxWaveState
{
	/** 当前波次下标（从 0 开始），-1 表示尚未开始。 */
	int32 WaveIndex = -1;

	/** 总波次数量。 */
	int32 TotalWaves = 0;

	/** 波次状态（宿主定义：0=未激活，1=等待开始，2=刷怪中，3=等待清怪，4=波间，5=完成）。 */
	int32 State = 0;

	/** 比赛结果（宿主定义：0=进行中，1=胜利，2=失败）。 */
	int32 Result = 0;

	/** 距下一个状态边界的秒数。 */
	double SecondsUntilNextBoundary = 0.0;
};

/**
 * IAkUGCSandboxHost
 *
 * 沙箱受控 API 的宿主接口。UGC 运行时（如权威玩法会话 / 逻辑运行时子系统）
 * 通过实现该接口，把受控能力注入沙箱，脚本得以在 `ugc` 命名空间下调用。
 *
 * 沙箱本身保持游戏无关：它只认识这个抽象接口，不直接依赖任何具体运行时。
 * 未注入 Host 时，沙箱脚本无法访问 `ugc` 命名空间（安全默认）。
 *
 * 沙箱在 Initialize 时以 TSharedPtr 持有 Host 强引用，因此 Host 既可以是
 * 独立对象，也可以由使用方以 TSharedPtr 管理，其生命周期只需满足：
 * 在沙箱 RunScript 期间有效即可。
 */
class AKUGCSANDBOX_API IAkUGCSandboxHost
{
public:
	virtual ~IAkUGCSandboxHost() = default;

	/** 受控消息：脚本调用 ugc.message(msg) 时触发。 */
	virtual void EmitMessage(const FString& Message) = 0;

	/**
	 * 受控实体查询：脚本调用 ugc.get_health(entityId) 时触发。
	 * 返回实体当前/最大血量；实体不存在或无血量状态时返回 false。
	 * 宿主未实现该能力时返回 false（沙箱侧视为「未找到」）。
	 */
	virtual bool QueryEntityHealth(const FString& EntityId, double& OutCurrent, double& OutMaximum)
	{
		return false;
	}

	/**
	 * 受控伤害：脚本调用 ugc.apply_damage(sourceId, targetId, damage) 时触发。
	 * 成功返回 true 并回填实际伤害 / 剩余血量 / 是否击杀；失败时 OutError 返回原因。
	 * 宿主未实现该能力时返回 false（沙箱侧抛错）。
	 */
	virtual bool ApplyDamage(
		const FString& SourceEntityId,
		const FString& TargetEntityId,
		double Damage,
		double& OutAppliedDamage,
		double& OutHealthAfter,
		bool& OutKilled,
		FString& OutError)
	{
		return false;
	}

	/**
	 * 受控生成：脚本调用 ugc.spawn(prefabId, [anchorId]) 时触发。
	 * 成功返回 true 并回填新实体 ID；失败时 OutError 返回原因。
	 * AnchorEntityId 可为空（表示无锚点）。宿主未实现该能力时返回 false（沙箱侧抛错）。
	 */
	virtual bool SpawnEntity(
		const FString& PrefabId,
		const FString& AnchorEntityId,
		FString& OutEntityId,
		FString& OutError)
	{
		return false;
	}

	/**
	 * 受控规则集查询：脚本调用 ugc.get_wave_state() 时触发。
	 * 回填波次/胜负快照；宿主未实现该能力时返回 false（沙箱侧视为「无规则集状态」）。
	 */
	virtual bool QueryWaveState(FAkUGCSandboxWaveState& OutState)
	{
		return false;
	}
};

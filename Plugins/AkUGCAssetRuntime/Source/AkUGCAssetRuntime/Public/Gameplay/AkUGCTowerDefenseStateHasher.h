#pragma once

#include "CoreMinimal.h"
#include "Subsystem/AkUGCLogicRuntimeSubsystem.h"

/**
 * 塔防玩法层最终状态快照。
 *
 * 仅包含「终局稳定」字段：跨平台、跨进程、跨时间步长都确定一致的属性。
 * 刻意排除 FAkUGCWaveRuntimeSnapshot::SecondsUntilNextBoundary 这类
 * 依赖推进步长切片的瞬态值，保证大 Delta 与小 Delta 到达同一终局时哈希一致。
 */
struct AKUGCASSETRUNTIME_API FAkUGCTowerDefenseFinalState
{
    // ---- 波次稳定字段（排除瞬态 SecondsUntilNextBoundary）----
    EAkUGCWaveRuntimeState WaveState = EAkUGCWaveRuntimeState::Inactive;
    EAkUGCTowerDefenseMatchResult MatchResult = EAkUGCTowerDefenseMatchResult::InProgress;
    int32 CurrentWaveIndex = INDEX_NONE;
    FGuid CurrentWaveId;
    int32 TotalWaveCount = 0;

    // ---- 基地生命 ----
    double BaseCurrentHealth = 0.0;
    double BaseMaximumHealth = 0.0;

    // ---- 活跃敌人数 ----
    int32 ActiveEnemyCount = 0;

    // ---- 事件序列（按权威时间线追加，顺序即确定）----
    TArray<FAkUGCLogicRuntimeMessage> Messages;
    TArray<FAkUGCLogicRuntimeSpawn> Spawns;
    TArray<FAkUGCLogicRuntimeGoalReached> Goals;
    TArray<FAkUGCLogicRuntimeDamage> Damages;
    TArray<FAkUGCLogicRuntimeDeath> Deaths;
};

/**
 * 塔防玩法层最终状态确定性哈希工具。
 *
 * 目标：对同一份 Logic Pack 与输入，无论平台（Win64 / Android / Dedicated Server）
 * 或推进方式（大 Delta / 小 Delta），都产生一致的 SHA-256 摘要。
 *
 * 确定性由「逐字段手动序列化」保证：
 *  - 枚举按底层整数值输出（不依赖枚举名）
 *  - GUID 按 32 位小写十六进制输出
 *  - double 按 IEEE 754 位模式输出（消除跨平台十进制格式化差异）
 *  - 事件数组按追加顺序输出（即权威时间线顺序）
 */
class AKUGCASSETRUNTIME_API FAkUGCTowerDefenseStateHasher
{
public:
    /** 对玩法层最终状态快照计算 SHA-256 hex（64 字符小写）。 */
    static FString HashFinalState(const FAkUGCTowerDefenseFinalState& State);
};

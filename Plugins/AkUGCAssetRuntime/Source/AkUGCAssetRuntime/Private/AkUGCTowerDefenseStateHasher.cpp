#include "Gameplay/AkUGCTowerDefenseStateHasher.h"

#include "Pack/AkUGCLogicPackHasher.h"

namespace
{
    /** 枚举按底层整数值输出，消除枚举名/平台差异。 */
    FString SerializeEnum(int32 Value)
    {
        return FString::Printf(TEXT("%d"), Value);
    }

    /** GUID 按 32 位小写十六进制输出（EGuidFormats::Digits），跨平台确定。 */
    FString SerializeGuid(const FGuid& Guid)
    {
        return Guid.ToString(EGuidFormats::Digits);
    }

    /** double 按 IEEE 754 位模式输出（高/低 32 位），消除跨平台十进制格式化差异。 */
    FString SerializeDouble(double Value)
    {
        uint64 Bits = 0;
        FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
        return FString::Printf(TEXT("%08x%08x"),
            static_cast<uint32>(Bits >> 32),
            static_cast<uint32>(Bits & 0xFFFFFFFFu));
    }

    FString SerializeInt(int32 Value)
    {
        return FString::Printf(TEXT("%d"), Value);
    }

    FString SerializeBool(bool Value)
    {
        return Value ? TEXT("1") : TEXT("0");
    }

    FString SerializeName(const FName& Name)
    {
        return Name.ToString();
    }

    void AppendMessage(FString& Out, const FAkUGCLogicRuntimeMessage& Msg)
    {
        Out += SerializeGuid(Msg.SourceNodeId);
        Out += TEXT(",");
        Out += Msg.Message;
    }

    void AppendSpawn(FString& Out, const FAkUGCLogicRuntimeSpawn& Spawn)
    {
        Out += SerializeGuid(Spawn.SourceNodeId);
        Out += TEXT(",");
        Out += SerializeGuid(Spawn.EntityId);
        Out += TEXT(",");
        Out += SerializeName(Spawn.PrefabId);
    }

    void AppendGoal(FString& Out, const FAkUGCLogicRuntimeGoalReached& Goal)
    {
        Out += SerializeGuid(Goal.SourceNodeId);
        Out += TEXT(",");
        Out += SerializeGuid(Goal.EntityId);
        Out += TEXT(",");
        Out += SerializeGuid(Goal.GoalEntityId);
        Out += TEXT(",");
        Out += SerializeGuid(Goal.BaseEntityId);
        Out += TEXT(",");
        Out += SerializeDouble(Goal.DamageApplied);
        Out += TEXT(",");
        Out += SerializeDouble(Goal.BaseHealthAfterDamage);
    }

    void AppendDamage(FString& Out, const FAkUGCLogicRuntimeDamage& Damage)
    {
        Out += SerializeGuid(Damage.SourceEntityId);
        Out += TEXT(",");
        Out += SerializeGuid(Damage.TargetEntityId);
        Out += TEXT(",");
        Out += SerializeDouble(Damage.RequestedDamage);
        Out += TEXT(",");
        Out += SerializeDouble(Damage.AppliedDamage);
        Out += TEXT(",");
        Out += SerializeDouble(Damage.HealthAfterDamage);
        Out += TEXT(",");
        Out += SerializeBool(Damage.bKilled);
    }

    void AppendDeath(FString& Out, const FAkUGCLogicRuntimeDeath& Death)
    {
        Out += SerializeGuid(Death.SourceEntityId);
        Out += TEXT(",");
        Out += SerializeGuid(Death.EntityId);
    }
}

FString FAkUGCTowerDefenseStateHasher::HashFinalState(const FAkUGCTowerDefenseFinalState& State)
{
    FString Canonical;
    Canonical.Reserve(2048);

    // 波次稳定字段（不含瞬态 SecondsUntilNextBoundary）
    Canonical += SerializeEnum(static_cast<int32>(State.WaveState));
    Canonical += TEXT("|");
    Canonical += SerializeEnum(static_cast<int32>(State.MatchResult));
    Canonical += TEXT("|");
    Canonical += SerializeInt(State.CurrentWaveIndex);
    Canonical += TEXT("|");
    Canonical += SerializeGuid(State.CurrentWaveId);
    Canonical += TEXT("|");
    Canonical += SerializeInt(State.TotalWaveCount);
    Canonical += TEXT("|");

    // 基地生命与活跃敌人数
    Canonical += SerializeDouble(State.BaseCurrentHealth);
    Canonical += TEXT("|");
    Canonical += SerializeDouble(State.BaseMaximumHealth);
    Canonical += TEXT("|");
    Canonical += SerializeInt(State.ActiveEnemyCount);

    // 事件序列（权威时间线顺序即确定顺序，不额外排序）
    Canonical += TEXT("|");
    Canonical += SerializeInt(State.Messages.Num());
    for (const FAkUGCLogicRuntimeMessage& Msg : State.Messages)
    {
        Canonical += TEXT(";");
        AppendMessage(Canonical, Msg);
    }

    Canonical += TEXT("|");
    Canonical += SerializeInt(State.Spawns.Num());
    for (const FAkUGCLogicRuntimeSpawn& Spawn : State.Spawns)
    {
        Canonical += TEXT(";");
        AppendSpawn(Canonical, Spawn);
    }

    Canonical += TEXT("|");
    Canonical += SerializeInt(State.Goals.Num());
    for (const FAkUGCLogicRuntimeGoalReached& Goal : State.Goals)
    {
        Canonical += TEXT(";");
        AppendGoal(Canonical, Goal);
    }

    Canonical += TEXT("|");
    Canonical += SerializeInt(State.Damages.Num());
    for (const FAkUGCLogicRuntimeDamage& Damage : State.Damages)
    {
        Canonical += TEXT(";");
        AppendDamage(Canonical, Damage);
    }

    Canonical += TEXT("|");
    Canonical += SerializeInt(State.Deaths.Num());
    for (const FAkUGCLogicRuntimeDeath& Death : State.Deaths)
    {
        Canonical += TEXT(";");
        AppendDeath(Canonical, Death);
    }

    return FAkUGCLogicPackHasher::Sha256Hex(Canonical);
}

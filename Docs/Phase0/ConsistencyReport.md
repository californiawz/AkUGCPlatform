# Phase 0 三平台一致性验收报告

> 对应 `ImplementationPlan.md` P10「Android、Dedicated Server 与一致性验收」。
> 本报告记录同一份 UGC Project Document 在 Win64 Client、Android Client、Win64 Dedicated Server 上一致运行能力的验证现状，明确已完成项与待真机验证项。

## 1. 三平台构建状态

| 平台 | 编译 | Cook | Package | 状态 |
|---|---|---|---|---|
| Win64 Client | ✅ Development | ✅ | ✅ | 可运行 |
| Android Client (arm64) | ✅ Development | ✅ ASTC | ✅ APK + OBB | 待真机 |
| Win64 Dedicated Server | ✅ | ✅ | ✅ | 可运行（Headless 已验证） |

- Android 工具链：NDK r27c `27.2.12479018`、SDK android-34 / build-tools 35.0.1 / cmake 3.22.1、JDK 17。
- Android 产物：`OutDir/AkUGCPlatform/<时间戳>/Android_ASTCClient/AkUGCPlatformClient-arm64.apk`。

## 2. 一致性验证结果

针对 `ImplementationPlan.md` 第 502–508 行的逐项比较，现状如下：

| 比较项 | 自动化验证 | 结果 | 覆盖测试 |
|---|---|---|---|
| Spawn 顺序 | ✅ 单平台确定性 | 通过 | `AkUGC.Core.Pack.SpawnDeterminism` |
| Logic 事件顺序 | ✅ 单平台确定性 | 通过 | `AkUGC.Core.Pack.ThreeWaveHeadless` |
| 最终状态哈希（Logic IR 层） | ✅ 内容确定性 + 文件往返一致 | 通过（64 位 SHA-256 hex） | `AkUGC.Core.Pack.ThreeWaveHeadless` |
| 路径位置与到达结果 | ✅ 单平台确定性（大/小时间步一致） | 通过 | `AkUGC.AssetRuntime.*` |
| Damage / Death | ✅ 单平台确定性 | 通过 | `AkUGC.AssetRuntime.*`（塔攻击/击杀） |
| Wave / Victory / Defeat | ✅ 单平台确定性 | 通过 | `AkUGC.AssetRuntime.*`（三波清空/基地归零） |
| Join-in-progress 恢复完整状态 | ✅ 端到端 | 通过 | `AkUGC.Runtime.GameMode.JoinInProgress*` |
| **跨平台哈希一致性（三平台同哈希）** | ⏳ 待真机 | 待验证 | — |

### 2.1 确定性验证说明

- **Logic IR 层**：同一 Document 两次独立构建 `ContentHash` 一致；同一 Release 文件往返后 `GameStart + 三波 WaveStart` 的运行结果哈希一致（`ComputeRunStateHash`，覆盖指令计数 / 消息 / Spawn 效果 / 延迟）。
- **完整玩法层**：`AkUGCLogicRuntimeSubsystemTests` 验证同一 Pack 以「大时间步」与「小时间步」推进到 Victory / Defeat，最终波次状态、基地生命、活跃敌人数一致，证明玩法逻辑与时间步长无关、具备确定性。
- **复制契约**：`JoinInProgressMidWave` / `JoinInProgressAfterDefeat` 验证权威会话运行到中途 / 终局后，`ProjectStateToGameState` 投影出的 GameState 快照能让新加入客户端完整恢复（波次状态、波序、总波数、胜负、基地生命、活跃敌人数）。

### 2.2 跨平台哈希一致性（缺口）

当前 `ComputeRunStateHash` 仅覆盖 **Logic IR 层**。玩法层（路径位置、伤害、死亡、胜负）的跨平台最终状态哈希尚未实现；且 Android 真机无法在无设备环境下产出对比数据。此项需在真机验证阶段完成。

## 3. 本次修复的关键缺陷

推进 P10「多 Client 连接与 Join-in-progress」验收时，端到端测试暴露两个此前被自建 Document 绕过的真实缺陷：

1. **GameMode 权威会话波次规则集从未启动**（`AkUGCPlayableSceneFactory.cpp`）
   - 根因：`MakePlayableTowerDefenseDocument` 的 LogicGraph 缺少 `WaveStart` 入口节点，编译器无法记录 `WaveStartEntryIndex`，导致 `Subsystem` 的 `WaveState.TotalWaveCount` 恒为 0、`CurrentWaveIndex` 恒为 -1。
   - 修复：补上 `WaveStart → Message` 节点，`waveStartEntryIndex` 由 -1 变为正确索引。

2. **Defeat 终局在推进时间边界上不确定**（`AkUGCPlayableSceneFactory.cpp`）
   - 根因：`enemy_spawn` 的 spawn point 在原点 `(0,0,0)`，敌人到终点的总路径距离约 1002.5，`moveSpeed=300` 需 3.34s 才抵达，与验收推进时间 3.0s 冲突。
   - 修复：spawn point 对齐到 `(250,50,0)`，总路径缩短为 750，2.5s 走完。

## 4. 自动化测试基线

- 完整 AkUGC 自动化套件 **92 项全部通过（0 失败）**（含 `AkUGC.Core.*`、`AkUGC.AssetRuntime.*`、`AkUGC.Runtime.*`）。
- 新增 `AkUGC.Runtime.GameMode` 4 项：`PackLoad`、`PackReject`、`JoinInProgressMidWave`、`JoinInProgressAfterDefeat`。

## 5. 待真机验证清单（Android）

以下项需物理 Android 设备，当前无法自动化：

1. 真机加载签名 Logic Pack（含本次重新生成的 `Saved/AkUGC/test_pack.json`，已含 `WaveStart` 与修正后的 spawn point）。
2. 创建、编辑、保存、重载文档。
3. Timer / Spawn / Path / Combat / Wave 实机运行。
4. 内存、帧时间、加载与卸载预算。
5. 跨平台最终状态哈希对比：同一 Pack 在 Android 与 Win64 Client、Win64 DS 上产出相同哈希（需先实现玩法层哈希，见 2.2）。

## 6. 结论

- 三平台（Win64 Client / Android Client / Win64 DS）的编译、Cook、Package 链路均已打通。
- Logic IR 层与完整玩法层的**单平台确定性**已通过自动化测试验证。
- 复制契约（Join-in-progress）已通过端到端测试验证，并修复了波次规则集未启动与 Defeat 边界不确定两个关键缺陷。
- 剩余工作集中在 **Android 真机验证** 与 **跨平台玩法层哈希一致性**，需物理设备与后续阶段推进。

# Phase 0 实施计划

## 1. 目标与验收边界

Phase 0 的目标是证明同一份 UGC Project Document 能够被 Desktop Creator 与移动端轻编辑器修改，编译为同一套 Logic IR，并在 Win64 Client、Android Client 与 Win64 Dedicated Server 上一致运行。

首个产品化垂直切片是“三波合作塔防”。Phase 0 只使用官方 Prefab，不开放玩家 C++、插件、任意 Blueprint、任意 UObject 反射、文件系统或外部网络访问。

最终验收必须同时满足：

- Project Document 是 Portable 项目的唯一真源。
- Desktop 与 App 的编辑操作产生相同的序列化 Command。
- Dedicated Server 负责生成、移动、伤害、死亡、波次和胜负等权威玩法结果。
- Client 不重复执行权威 Logic，只接收和展示复制状态。
- 同一 Release 与输入在 Win64、Android 和 Dedicated Server 上得到一致的规则结果。
- UGC Logic 受到 Capability、指令、时间、内存、实体和资源预算限制。
- Logic Pack 具有确定性哈希和可信签名，运行端拒绝被修改或不兼容的包。
- Android 能在目标设备预算内完成下载、加载、运行、卸载和作品切换。

## 2. 当前实现基线

### 2.1 已完成

#### Project Document

- 当前 Schema 为 V3。
- 支持 Manifest、Scene、Entity、PrefabId、Transform、父子层级、Component Data、类型化 Property、Logic Graph 和 Capability 名称列表。
- 支持 JSON 往返、int64 无损编码、非有限数值拒绝、全局 EntityId 唯一性和层级循环校验。
- 支持 V0 → V1 → V2 → V3 连续迁移。
- Desktop 保存使用临时文件回读验证与原子替换。

#### Prefab 与 Schema

- 已注册 11 个官方塔防 Prefab。
- 已定义基地、刷怪点、路径点、终点、塔位、敌人和基础塔的数据属性。
- 支持默认 Component 实例化、属性类型与范围、移动端编辑权限、放置规则和成本预算。

#### Command 与编辑事务

- Entity 命令：Add、Delete、Duplicate、SetTransform、SetProperty、RemoveProperty、SetParent。
- Logic 命令：AddLogicNode、DeleteLogicNode、ConnectLogicNode、DisconnectLogicNode。
- 支持事务原子性、JSON、Undo/Redo 和 Runtime 增量投影。
- Desktop 与 App 共用 Runtime Command Service。

#### Creator Studio

- 支持新建、加载、安全保存、官方 Prefab 放置、Gizmo Transform 回写、复制、删除和父子层级编辑。
- 支持 Entity 树与 Level Editor 视口双向选择同步。
- 支持 Schema 驱动属性面板和 Parent 编辑。
- UE 原生 Scene Outliner 删除、Attach、Detach、Reparent 能回写 Document。

#### App 轻编辑后端

- Blueprint WorldSubsystem 支持项目创建、JSON 导入导出、Prefab 枚举、Entity 放置/删除/复制、Transform、Parent、Property 和 Undo/Redo。
- 支持移动端只读属性保护、Prefab 放置约束和内容预算。
- 尚无正式移动端 UMG 产品界面。

#### Logic Graph 与 Runtime

- 节点：GameStart、Message、Timer、Spawn。
- Compiler 执行统一校验、稳定拓扑排序，并生成包含入口和后继索引的确定性 Logic IR。
- Runner 支持 Message、Timer 暂停、延迟续跑和 Spawn Effect。
- World Logic Runtime 支持自动 Tick、测试用虚拟时间推进、累计指令预算和重入保护。
- Logic Spawn 只在权威端执行，不写回作者 Document。
- enemy_spawn 的 enemyPrefab、enemyCount、spawnInterval 已能驱动确定性批次生成。
- Session 与 Scene Runtime 生命周期结束时会取消未完成 Timer 和 Spawn Batch。
- Runtime Session 已显式区分 Edit、Preview、PlayAuthority 和 PlayClient。
- Creator Studio 与 App Editor 使用 Edit，不会在加载项目时执行 GameStart。
- Logic Runtime 按 Session Owner 隔离；其他 Edit 会话的加载和析构不会取消当前玩法执行。
- Preview 与 PlayAuthority 会校验 World/Authority 契约，不兼容时明确失败。

#### 当前验证基线

- AkUGCPlatformEditor Win64 Development 编译成功。
- AkUGCPlatformClient Win64 Development 编译成功。
- AkUGCPlatformServer Win64 Development 编译成功。
- 41 项 AkUGC 自动化测试全部通过，零自动化警告。

### 2.2 尚未完成

- 正式 Player/Dedicated Server 项目加载入口尚未接入 PlayAuthority/PlayClient；当前模式基础设施和测试已完成。
- 敌人没有沿 path_node 移动。
- Goal、基地伤害、运行时生命、塔攻击和死亡未实现。
- WaveStart、三波状态机和胜负未实现。
- Creator/App 尚不能编辑 Logic。
- Runtime Entity 和塔防状态尚无完整多人复制契约。
- Logic Pack、依赖、哈希、签名和可信加载未实现。
- Lua 5.3.4 独立沙箱未实现。
- Android 真机、Dedicated Server 联机和跨平台一致性验收未完成。
- 官方 Prefab 大多没有正式 AssetVariants，当前运行时表现主要是通用 Actor。

## 3. 实施原则

1. 每次只推进一个可以自动化验证的最小垂直切片。
2. Project Document 始终是作者内容真源；玩法运行时状态不得回写作者 Document。
3. UI、Slate、UMG、AI 和网络层不得直接修改 Document，必须通过统一 Command。
4. 权威玩法只在 PlayAuthority 会话执行；Client 不本地生成权威结果。
5. 确定性排序必须使用稳定 ID 或显式 Order，不能依赖 TMap、Actor 或数组偶然顺序。
6. 每个异步任务必须有明确所有权、取消路径、预算和失败终态。
7. 新增持久化字段必须升级 Schema 并提供迁移和兼容测试。
8. 新增节点必须同时完成 Document、Validator、Command、Compiler、Runner、Runtime Effect 和测试闭环。
9. 每个提交必须通过 Editor、Client、Server 编译、完整 AkUGC 测试、Lint 和 git diff --check。
10. Android、网络复制、Lua 和签名不使用临时绕过方案降低安全门禁。

## 4. 分阶段实施计划

## P0：编辑会话与玩法会话隔离（基础设施已完成）

### 状态

- Edit、Preview、PlayAuthority 和 PlayClient 模式已落地。
- Creator Studio 与 App Editor 已显式接入 Edit。
- Session Owner 隔离和冲突拒绝已落地。
- 正式玩法加载器接入 PlayAuthority/PlayClient 归入 P5 联机运行入口。

### 目标

显式区分 Edit、Preview、PlayAuthority 和 PlayClient，避免 Creator Studio 或 App Editor 加载带 Logic Graph 的作品时自动执行 GameStart。

### 核心任务

- 为 FAkUGCDocumentRuntimeSession 增加明确的 Session Mode。
- 默认模式设为 Edit，保证既有编辑入口安全。
- Creator Studio 和 App Editor 显式或默认使用 Edit。
- 自动玩法测试与正式玩法加载使用 PlayAuthority。
- Preview 允许本地预览执行，但不能替代正式权威服务器语义。
- PlayClient 只加载场景投影，不执行权威 Logic。
- Session 模式不得继续依赖 WorldType 或 NetMode 的偶然组合推断。

### 验收

- Creator Studio 加载带 GameStart/Spawn 的项目不生成敌人。
- App Editor 在 Game World 加载同一项目也不生成敌人。
- PlayAuthority 初始化后自动执行 GameStart。
- PlayClient 初始化后不执行 GameStart。
- Edit 模式仍支持 Command、Undo/Redo 和 Runtime Actor 预览。

### 建议提交

`重构：区分 UGC 编辑会话与玩法运行会话`

## P1：路径构建与敌人确定性移动（已完成）

### 状态

- 已从 Scene Document 提取并按 order、EntityId 稳定排序 path_node。
- 已校验 Order 类型、范围、重复值、重复 EntityId 和最小路径段长度。
- Edit 支持 0/1 节点中间状态；包含 Spawn 玩法的 Preview/PlayAuthority 要求至少两个路径点。
- 放置和复制 path_node 会自动分配最小可用 Order。
- Scene Load、同步、Command、Undo/Redo 后会更新派生路径。
- Logic Spawn 的 Basic Enemy 会读取 moveSpeed 并注册权威移动状态。
- Logic Runtime 按最近 Timer/Spawn 事件切片推进移动，新生成敌人只移动事件后的剩余时间。
- 支持大 Delta 跨越多个路径段，并按 EntityId 稳定顺序推进多个敌人。
- 到达最后路径点后停止移动并产生一次性 GoalReached 事件，暂不结算基地伤害。

### 目标

让 Basic Enemy 按 path_node.order 从刷怪点沿路径移动到 Goal。

### 核心任务

- 收集当前 Scene 中的 official.gameplay.path_node。
- 读取 tower_defense.path_node.order，并按 Order、EntityId 稳定排序。
- 拒绝重复 Order、非法类型、缺失节点和无效路径。
- 为生成的敌人维护当前路径段、段内进度和 moveSpeed。
- 仅 PlayAuthority 推进移动；Client 使用复制结果。
- 支持大 Delta 跨越多个路径段，保证大步进与小步进终态一致。
- Session/场景卸载时取消移动状态。
- 到达最后节点时先产生确定性 GoalReached 事件，不在本阶段结算伤害。

### 涉及模块

- AkUGCAssetRuntime Scene Runtime。
- 新增或扩展塔防玩法 Runtime Subsystem。
- Entity Binding 与 Runtime Actor。
- Runtime 自动化测试。

### 验收

- 路径顺序不依赖 Actor/TMap/数组偶然顺序。
- 敌人在给定时间到达精确位置。
- 大 Delta 与小 Delta 结果一致。
- Client 不独立推进权威移动。
- Document 保持不变。

### 建议提交拆分

1. `功能：构建并校验塔防路径`
2. `功能：驱动敌人沿路径确定性移动`

## P2：Goal 与基地伤害

### 目标

敌人到达终点后对基地造成 goalDamage，并从运行时移除。

### 核心任务

- 定义唯一 Goal/Base 的查找和校验规则。
- 建立独立 Runtime Health 状态，初值来自 core.health.maxHealth。
- 从敌人 tower_defense.enemy.goalDamage 读取伤害。
- GoalReached 只结算一次伤害。
- 到达敌人停止移动并安全移除。
- 维护活跃敌人数和基地生命。
- 为后续网络复制暴露只读状态。

### 验收

- 到达一次只扣除一次生命。
- 大 Delta 不重复结算。
- 敌人移除后不再参与移动和战斗。
- 作者 Document 不发生变化。

### 建议提交

`功能：结算敌人到达目标与基地伤害`

## P3：基础塔攻击、伤害与死亡

### 目标

Basic Tower 能稳定选择目标、按攻击间隔造成伤害并移除死亡敌人。

### 核心任务

- 建立 Runtime Health、Damage 和 Death Effect。
- 读取 attackRange、attackInterval、attackDamage、maxHealth。
- 按路径进度优先、EntityId 作为 Tie-break 选择目标。
- 只在权威端执行攻击与伤害。
- 正确处理大 Delta、多次攻击间隔和死亡去重。
- 敌人死亡后停止移动、从运行时移除并更新活跃计数。

### 明确暂缓

- 炮弹 Actor、复杂碰撞、动画、Buff/Debuff、元素系统。

### 建议提交拆分

1. `功能：新增运行时生命与伤害模型`
2. `功能：新增基础塔确定性攻击`

## P4：三波状态机、WaveStart 与胜负

### 目标

形成可完成的三波合作塔防规则闭环。

### 数据设计

升级 Project Document 至 V4，引入 Ruleset：

- 三波配置。
- 每波 Spawn Point、Enemy Prefab、Count、Interval、Start Delay。
- 波次间隔。
- 基地失败条件与第三波胜利条件。

### Logic 扩展

- WaveStart 事件入口。
- WaveCompleted 事件。
- Compiler 支持多个事件入口和稳定寻址。
- Runner 支持按事件及 WaveIndex 启动。

### 状态机

```text
WaitingToStart
→ Spawning
→ WaitingForEnemies
→ Completed
→ NextWave
→ Victory
```

任意阶段基地生命归零则进入 Defeat。Victory 与 Defeat 必须互斥且只能触发一次。

### 建议提交拆分

1. `功能：新增 V4 塔防 Ruleset 与迁移`
2. `功能：新增 WaveStart Logic 事件入口`
3. `功能：实现三波权威状态机`
4. `功能：实现塔防胜负判定`

## P5：多人合作与状态复制

### 目标

Dedicated Server 权威运行，多个 Client 观察一致状态，并支持中途加入。

### 核心任务

- 新增 UGC GameMode/GameState。
- 复制当前波次、波次状态、基地生命、活跃敌人数和胜负状态。
- 定义 Runtime Entity 稳定复制标识和销毁协议。
- Client 禁止执行 Spawn、Movement、Damage 和 Wave 状态机。
- Join-in-progress 恢复当前场景与玩法状态。

### 验收矩阵

- 单机 PIE。
- Listen Server + Client。
- Dedicated Server + 1 Client。
- Dedicated Server + 2 Client。
- 中途加入 Client。

### 建议提交

`功能：复制塔防权威状态与运行时实体`

## P6：Creator Studio Logic 编辑

### 前置任务

新增 UpdateLogicNode 或 SetLogicNodeParameter Command，支持 Message、Delay、Spawn Prefab 和 Spawn Anchor，且必须可序列化、撤销和重做。

### UI 顺序

先实现列表式编辑器：

1. Logic Node 列表。
2. 新增与删除节点。
3. 参数 Details。
4. Source/Target 下拉连接。
5. Validator 错误显示。
6. Undo/Redo。

运行语义稳定后再实现完整节点画布、Pin 和布局持久化。

### 建议提交拆分

1. `功能：新增 Logic Node 参数修改命令`
2. `功能：新增 Creator Studio Logic 列表编辑器`
3. `功能：新增 Trigger Graph 可视化画布`

## P7：App Logic 轻编辑

### 目标

App 通过模板化方式编辑安全的有限 Logic，不提供完整专业节点画布。

### 建议能力

- 开始后延迟刷怪模板。
- 分波刷怪模板。
- 消息提示模板。
- 可编辑 Delay、Enemy Prefab、Count、Interval 和 Spawn Point。
- 禁止任意循环、Lua、Desktop-only 属性和超预算节点。

### 验收

- App 与 Desktop 产生相同序列化 Command。
- JSON 往返一致。
- 移动端权限和预算严格生效。
- 恶意 JSON 与越权参数被拒绝。

## P8：Logic Pack、Manifest、哈希与签名

### 目标

形成可发布、不可变且可信加载的作品包。

### Pack 内容

- Release Manifest。
- Scene Document。
- Logic IR。
- Ruleset。
- Asset Dependencies。
- Budget 与 Capability。
- 文件哈希和数字签名。

### 核心任务

- 确定性序列化。
- Pack Builder 与 Validator。
- ReleaseId 和哈希清单。
- 签名与验签。
- Client/Server 加载同一 Pack。
- 拒绝被修改、超预算或版本不兼容的 Pack。

### 建议提交拆分

1. `功能：新增 Logic Pack Manifest 与确定性哈希`
2. `功能：新增 Logic Pack 构建和加载`
3. `功能：新增发布签名与验签`

## P9：Lua 5.3.4 独立沙箱

### 目标

提供 L3 脚本能力，但不替代 L2 Trigger Graph。

### 核心任务

- 集成 Lua 5.3.4。
- 每个作品独立 VM。
- 禁止文件系统、网络、OS、动态库和任意 UObject 访问。
- 只开放 Entity Query、Timer、Message、Spawn、Damage 和 Ruleset 等受控 API。
- 实施指令、时间、内存、调用深度和 Effect 配额。
- Capability/Effect Validator 在 Client 和 Server 侧重复校验。
- 死循环、超时和 OOM 能安全终止而不影响宿主。

### 验收

- 越权 API 不存在。
- 无限循环会被预算终止。
- 不同作品 VM 隔离。
- Client 不执行权威脚本。

## P10：Android、Dedicated Server 与一致性验收

### Android

- 验证 SDK/NDK。
- Android ASTC 编译、Cook、Package。
- 真机加载 Logic Pack。
- 创建、编辑、保存和重载。
- Timer、Spawn、Path、Combat 和 Wave 实机运行。
- 内存、帧时间、加载和卸载预算。

### Dedicated Server

- Server Package。
- 自动加载签名 Pack。
- Headless 三波塔防测试。
- 多 Client 连接与 Join-in-progress。
- 非法 Pack 拒绝。

### 一致性报告

对同一 Document、Release 和输入比较：

- Spawn 顺序。
- Logic 事件顺序。
- 路径位置与到达结果。
- Damage、Death、Wave、Victory/Defeat。
- 最终状态哈希。

目标平台：Win64 Client、Android Client、Win64 Dedicated Server。

## 5. 推荐执行顺序

```text
P0 会话模式隔离
  ↓
P1 路径构建与敌人移动
  ↓
P2 Goal 与基地伤害
  ↓
P3 塔攻击、伤害与死亡
  ↓
P4 三波状态机与胜负
  ↓
P5 多人状态复制
  ↓
P6/P7 Desktop 与 App Logic 编辑
  ↓
P8 Logic Pack、哈希与签名
  ↓
P9 Lua 沙箱
  ↓
P10 Android/DS 一致性验收
```

## 6. 近期提交计划

1. `重构：区分 UGC 编辑会话与玩法运行会话`（已提交）
2. `功能：构建并校验塔防路径`（已提交）
3. `功能：驱动敌人沿路径确定性移动`（已开发，待提交）
4. `功能：结算敌人到达目标与基地伤害`
5. `功能：新增运行时生命与伤害模型`
6. `功能：新增基础塔确定性攻击`
7. `功能：新增 V4 塔防 Ruleset 与迁移`
8. `功能：新增 WaveStart Logic 事件入口`
9. `功能：实现三波权威状态机`
10. `功能：实现塔防胜负判定`

每个提交完成后执行：

- AkUGCPlatformEditor Win64 Development 编译。
- AkUGCPlatformClient Win64 Development 编译。
- AkUGCPlatformServer Win64 Development 编译。
- 完整 AkUGC 自动化测试。
- Lint。
- git diff --check。

## 7. 明确暂缓

在本地三波塔防闭环完成前，暂缓：

- 云端控制面、社区推荐、结算和大规模服务编排。
- 玩家上传资产、插件、C++ 和任意 Blueprint。
- 完整手机专业编辑器。
- 多人实时协作编辑。
- 复杂地形、开放世界和完整 RPG/MMO。
- 完整 Logic Graph 画布优先于运行语义。
- Lua 优先于 Trigger Graph 的玩法闭环。

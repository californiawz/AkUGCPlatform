# Phase 1 实施计划

> 基线日期：2026-09-09
> 前置：Phase 0（P0–P10）已完成并提交，PC 侧闭环；Android 真机项延后。
> 本阶段目标：把 Phase 0 的「三波合作塔防逻辑闭环」推进为「非程序员可创作、可游玩、可分享的完整垂直切片」。

---

## 1. 目标与验收边界

### 1.1 目标

单模板（塔防）垂直切片，打通「创作 → 游玩 → 分享」完整链路：

```text
选模板 → L1 参数改规则 → 搭地图（可选）→ 本地单人（含 Bot）
→ 2–4 人私测联机 → 一键发布 → 他人加载游玩
```

### 1.2 验收门禁（Go/No-Go）

- **非程序员 60 分钟内**完成并分享首个可玩作品（L1 参数 + 少量摆放，不碰 Logic Graph）。
- **10 个种子作品可稳定联机**（2–4 人，无严重掉线/不同步）。
- 客户端与服务端计分、胜负结果**严格一致**（继承 Phase 0 确定性约束）。
- 发布包可被他人加载游玩，且**验签拒绝被篡改包**。

### 1.3 明确不做（本阶段）

- 云端项目/版本/审核/分发（Phase 2 起）。
- 玩家上传资产（Asset Pack，Phase 2 白名单试点）。
- iOS Player、完整手机专业编辑器。
- L3 受限 Lua Beta（Phase 3 起）。
- 现金结算、创作者经济。

---

## 2. 现状基线（Phase 0 成果 → Phase 1 缺口）

### 2.1 已可复用（Phase 0 产出）

| 能力 | 现状 |
|---|---|
| Project Document V6 + Ruleset | 波次/刷怪/属性唯一真源；P11 新增资源，V5 及更早编辑文档可迁移 |
| Command / Undo / Redo | ✅ Desktop 与 App 共用 Runtime Command Service |
| Logic Graph IR + Runner | ✅ 确定性编译与执行 |
| 三波塔防逻辑闭环 | ✅ 波次状态机、刷怪、路径、伤害、死亡、胜负 |
| 多人复制 | ✅ GameMode/GameState、Runtime Entity 复制、Join-in-progress |
| Logic Pack 构建/加载/签名 | ✅ FAkUGCLogicPackHasher/Builder/Loader + Ed25519 |
| Lua 沙箱（L3） | ✅ slua_unreal + Capability/Effect 校验 |
| Creator Studio | ✅ 场景摆放、Logic/Ruleset 编辑（偏专业） |
| App 轻编辑后端 | ✅ Logic/Ruleset 模板化命令 + 移动端权限/预算 |
| Win64 Editor/Client/Server 编译 + 全量自动化测试 | ✅ |

### 2.2 Phase 1 缺口（需新增）

| 缺口 | 现状 | Phase 1 目标 |
|---|---|---|
| L1 参数化模板 | 编辑入口偏专业（Logic/Ruleset） | 非程序员参数表单即可配置规则 |
| 正式 AssetVariants | 字段已定义，基本为空；运行时通用 Actor | 塔/敌人/基地有正式视觉资产 |
| HUD 与计分 | 源码零 HUD/UMG/Score | 完整对局 UI + 权威计分 |
| Bot 与单人循环 | 仅 Headless 测试，无 Bot 玩家 | 单人（含 Bot）完整对局 |
| 2–4 人联机私测 | 有复制基础，无房间/大厅 | 稳定多客户端联机 |
| 本地发布与分享 | 有 Pack 格式，无发布分享闭环 | 一键发布 + 他人加载 |
| 基础 AI Copilot | 有 UnrealAgent/MCPServer 骨架 | 自然语言生成结构化玩法 |

---

## 3. 实施原则（继承 Phase 0 + 新增）

继承 Phase 0 全部原则，另补：

1. **UI/UMG 不得直接改 Document**，必须通过统一 Command（与 Creator Studio 一致）。
2. **计分只在权威端结算**，Client 只展示复制结果。
3. **正式资产按分平台 Cook**，Android 资产预算独立校验。
4. **Bot 决策必须确定性可复现**（利于测试与一致性验收）。
5. **AI Copilot 只产出结构化变更 + 预览 Diff**，不直接执行任意代码。
6. **发布分享链路必须端到端验签**，不因「本地」而放松安全门禁。

---

## 4. 分阶段实施计划

### P11：L1 参数化模板（创作入口）

**目标**：非程序员通过参数表单即可配置塔防规则，产出可运行作品。

**核心任务**：
1. 定义塔防 L1 参数 schema（波次数、每波敌人类型/数量/间隔、敌人生命/速度/伤害、初始资源、基地生命、胜负条件）。
2. 参数 → 生成 Ruleset + enemy_spawn + 属性（走统一 Command，Document 保持唯一真源）。
3. 参数校验：类型、范围、内容预算、波次上限。
4. 参数面板 UI（Creator Studio 简单表单，不暴露 Logic Graph）。
5. 参数 JSON 往返 + Undo/Redo + Schema 迁移。

**验收**：改波次/血量/速度参数 → 生成可运行作品；参数校验与预算严格生效。

**建议提交**：`功能：新增塔防 L1 参数化模板与参数面板`

**实施状态（2026-10-10）**：已实现并通过 Win64 自动化验收。
- 共享参数 Schema V1；参数 V0→V1，Document V5→V6；严格 JSON 类型、范围、规范字段与重复键校验。
- 默认场景包含基地、终点、路径、两座塔、三组独立刷怪点和事件入口。Creator 默认简单表单，App 共用参数命令。
- 1–3 波、逐波敌人数量/间隔/生命/速度/到达伤害、基地生命与资源均被运行时消费；一次事务可完整 Undo/Redo。
- 内容预算计入静态实体、各波敌人及可达 Logic Spawn；减少波数保留作者实体，旧共享刷怪点拆分保留作者位置。
- 修复持续刷怪时以活跃敌人数生成 ID 导致碰撞，以及终点到达与多塔同时间结算的运行错误。成功生成 ID 改为单调计数；P10 Victory 哈希因此更新，但三 target 与大小步推进仍一致。
- Editor/Client/Server Win64 Development 编译成功；Editor 全量 AkUGC **101/101** 通过；Client/Server 各执行两项塔防 GameMode 测试并通过，标准场景及 1/2/3 波参数场景哈希与 Editor 一致。
- 证据：`Saved/Logs/P11BuildEditorGate.log`、`P11BuildClientGate.log`、`P11BuildServerGate.log`、`P11TestsGate.log`、`P11ClientGate.log`、`P11ServerGate.log`；定向 Lint 与 `git diff --check` 通过。
- 边界：Android 真机与非程序员人工操作验收尚未执行。旧 V5 签名发布包直接拒绝，不自动迁移或重签；编辑文档可迁移后重新发布。

---

### P12：正式 AssetVariants 与运行时绑定

**目标**：塔防 Prefab 有正式视觉资产，运行时真实渲染。

**核心任务**：
1. 为官方 Prefab（base/enemy/tower/spawn/path_node/goal）填充 AssetVariants（Mesh/材质/粒子）。
2. 运行时按 AssetVariant 实例化视觉表现（替代通用 Actor 占位）。
3. 分平台资产（Win64/Android 的 Mesh LOD、材质降级）。
4. 资产预算（Render/Physics 单元）接入既有 Cost 校验。

**验收**：作品运行时看到正式塔防视觉；Android 能加载对应资产并满足预算。

**建议提交**：`功能：新增塔防官方 Prefab 正式资产变体与运行时绑定`

**实施状态（2026-10-10）**：视觉接入基础子步骤已实现并通过 Win64 测试；P12 正式资产验收未完成。
- 官方基地/敌人/塔/刷怪点/路径点/终点填入 Win64、Android、Default 的 Engine BasicShapes 占位变体，Server 显式为空；可见标记纳入 Render 成本。
- `PrefabId` RepNotify 与客户端 BeginPlay 按本机平台恢复官方 StaticMesh，空标识清理旧 Mesh；新增自动化覆盖六类资产实际加载、回调恢复、平台路径和 Server 不加载视觉。
- `/Engine/BasicShapes` 加入显式 Cook 根。本轮 Win64 Client/Server 均执行非跳过的 Build/Cook/Stage/Package 并成功，不再沿用旧 Cook 内容替换二进制作为验收。
- 新增 `Tools/Tests/verify_network_visuals.py`，生成签名前联网夹具、启动真实 Dedicated Server 与两个独立 Client；第二客户端在启动后 27.44 秒、首客户端已观察到敌人后加入。两端均收到 11 个实体、资源321、同一敌人 GUID，验证 Mesh 注册/可见、敌人移动与销毁、Victory、基地生命993；Server 实测两个连接且无 Mesh 加载。使用 NullRHI 验证对象与网络，不等于截图或美术效果验收。
- 真实跨进程验收发现并修复 FName/反射属性显示大小写造成的内容哈希差异：名称和字段键按小写规范化，FString 内容仍大小写敏感。Editor/Client/Server 的固定文档哈希一致；旧算法签名包需由持有私钥的发布方重新构建签名，运行端不放松完整性或验签、不自动重签。
- PackTool 写出之前新增完整 JSON 回环验签，显式联网夹具输出确定性诊断表示；Dedicated Server 运行模式禁止回退 Default 加载视觉。
- Editor/Client/Server Development 构建成功；Editor 全量 **103/103** 通过；新 Cook Client/Server 的内容哈希回归各1项通过，真实网络测试 Client两次与Server一次均完成成功。联网测试只在显式 `-AkUGCNetworkVisual` 与配套夹具下执行，不计作常规单进程通过。
- 证据：`Saved/NetworkVisualVerification/result.json`、`cook_client.log`、`cook_server.log`、`server.log`、`client1.log`、`client2_late.log`；`Saved/Logs/P12NetworkEditorGate.log`、`P12CanonicalClient.log`、`P12CanonicalServer.log`。定向 Lint 与 `git diff --check` 通过。
- 同时修复 P11 提交后 adaptive build 回归 unity 模式时的模板辅助函数命名冲突。
- 本轮执行教训：首次误将 UAT `-clean` 与 Editor 测试并行，导致引擎模块文件锁与部分清理；已移除不必要清理、串行完成恢复构建，再独立跑全量测试。引擎已恢复正常。
- 待完成：有授权的正式 Mesh/材质、Android LOD/材质降级与真机验收、非 NullRHI 视觉效果验收。不得把 Engine 占位或网络对象测试标为正式美术验收。

---

### P13：HUD 与权威计分

**目标**：对局有完整 HUD 反馈 + 权威计分。

**核心任务**：
1. 定义计分模型（击杀得分、通关评价、个人得分）。
2. 权威计分状态机（仅 Server 结算，ReplicatedUsing + OnRep）。
3. HUD：波次进度、基地生命、资源/金币、得分、胜负结算。
4. 结算 UI（Victory/Defeat + 评分）。
5. 计分纳入最终状态哈希（扩展 FAkUGCTowerDefenseStateHasher）。

**验收**：对局有完整 HUD + 计分；客户端与服务端计分/胜负严格一致。

**建议提交**：`功能：新增权威计分与塔防 HUD`

---

### P14：Bot 与单人完整对局

**目标**：单人（含 Bot）完整打完一局。

**核心任务**：
1. Bot 玩家：确定性 AI 自动布塔/参与，决策可复现。
2. 完整对局循环：准备 → 对局 → 结算 → 返回。
3. 单机自托管（Standalone/Listen Server）。

**验收**：单人（含 Bot）完整打完一局，具备准备/对局/结算流程；Bot 决策可复现。

**建议提交**：`功能：新增确定性 Bot 与单人完整对局循环`

---

### P15：2–4 人联机私测

**目标**：多客户端稳定联机。

**核心任务**：
1. 房间/大厅流程（建房、加入、开始、退出）。
2. 多客户端联机（复用 Phase 0 复制基础）。
3. 断线重连与中途加入（复用 Join-in-progress）。

**验收**：2–4 人稳定联机完成对局；10 个种子作品可稳定联机（门禁）。

**建议提交**：`功能：新增房间大厅与 2–4 人联机私测`

---

### P16：本地发布与分享

**目标**：一键发布 + 他人加载。

**核心任务**：
1. 一键发布（打包 Logic Pack + 生成分享载体）。
2. 分享与加载（他人加载分享包，端到端验签）。
3. 本地作品库（浏览、导入、删除）。

**验收**：发布后可被他人加载游玩；验签拒绝被篡改包。

**建议提交**：`功能：新增本地发布、分享与作品库`

---

### P17：基础 AI Copilot

**目标**：自然语言生成结构化玩法 + 报错解释。

**核心任务**：
1. 自然语言 → 结构化玩法（建图、规则），产出受控 Command/Document 变更。
2. 编译器验证 + 预览 Diff（可撤销）。
3. 报错解释（编译/验证错误 → 自然语言建议）。

**验收**：AI 生成可编译验证的玩法；错误有可读解释。

**建议提交**：`功能：新增基础 AI Copilot（建图/规则/报错解释）`

---

## 5. 推荐执行顺序

```text
P11 L1 参数化模板
  ↓
P12 正式 AssetVariants
  ↓
P13 HUD 与权威计分
  ↓
P14 Bot 与单人完整对局
  ↓
P15 2–4 人联机私测
  ↓
P16 本地发布与分享
  ↓
P17 基础 AI Copilot
```

依赖说明：
- P12（视觉）与 P13（HUD）相对独立，可并行推进。
- P14 依赖 P13（对局要有 HUD/计分才算完整）。
- P15 依赖 P14（先单人闭环，再多端）。
- P16 依赖 P15（先联机稳定，再发布）。
- P17 可延后到最后，不阻塞「可玩作品」主链路。

---

## 6. 关键设计决策（待确认）

推进前建议确认以下四点，会影响具体实现：

1. **计分模型**：合作通关（团队总分 + 通关评价）vs 个人击杀得分（塔防内竞技）？
2. **Bot 定位**：塔防是合作 PvE，Bot 是「补位队友」还是仅「观战/陪玩」？
3. **AI Copilot 的 LLM 接口**：本地模型 vs 云端服务？走 UnrealAgent/MCPServer 哪条链路？
4. **发布分享形态**：本地文件拷贝 vs 作品码 + 本地索引？（Phase 1 不引入后端，倾向本地）

---

## 7. 近期提交计划

P11 已提交为 `b3a2699`：塔防 L1 参数化模板与参数面板；下一推进项为 **P12（正式 AssetVariants 与运行时绑定）**。

P12 当前 Content 没有可复用的 Mesh/材质资产。先接入 Engine 基础形状占位并验证复制回调、平台选择与内容预算；占位不计作正式资产或 Android 分平台降级验收。正式美术与 Android 真机验收保持待完成。

每个提交前执行（继承 Phase 0 门禁）：
- AkUGCPlatformEditor/Client/Server Win64 Development 编译。
- 完整 AkUGC 自动化测试。
- Lint + git diff --check。

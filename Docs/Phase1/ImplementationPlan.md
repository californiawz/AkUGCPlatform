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
- 用户于 2026-10-11 明确正式美术资产后续提供：正式 Mesh/材质、Android LOD/材质降级与真机验收延后，不阻塞 P13/P14；P12 技术接入按现有占位继续，不能标记正式美术验收完成。
- 非 NullRHI 客户端已在 P13 产出实际 HUD 结算截图；地图仍无正式相机/照明和美术，这不等于场景视觉验收。

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

**实施状态（2026-10-11）**：团队权威计分、HUD 与结算已实现并通过 Win64 自动化及真实客户端验证；个人归属分延后。
- 当前规则：官方敌人唯一有效死亡 +100 团队分、+10 资源；Victory 一次性 +500；评价展示终局基地保留生命百分比，不虚构等级阈值。基地死亡、到达销毁不算击杀。
- 共享计分累加器按敌人 GUID 去重；终局快照冻结，重复投影不发奖。计分只在 GameMode 权威端消费事件；GameState 增加权威校验。
- `FAkUGCTowerDefenseScoreSnapshot` 包含分数/资源/击杀/胜负/结算标记/冻结基地生命，使用自定义原子 NetSerialize 与 RepNotify；解决普通结构体子字段复制时终局标记先到的问题。
- 默认 GameMode 接入 Canvas HUD，无须 UMG/Blueprint/正式美术：波次、敌人数、基地生命条、资源、团队分、胜负与结算评价。结算必须读取完整计分快照。
- 计分全部字段纳入玩法终态哈希；标准 Victory/Defeat、1/2/3 波参数场景在 Editor/Client/Server 和大步/小步推进下哈希一致。
- 最新 Editor 全量 **107/107** 通过；Client/Server 各3项计分测试通过、各2项终态哈希测试通过；Development Build/Cook/Stage/Package 均成功。
- 真实 DS 与两个 Client（含晚加入）结算一致：Score500、Resources321、Kills0、Base993、Settled1。另以非 NullRHI 客户端捕获两张真实 HUD PNG，截图文件已实际产出并可打开；场景相机/照明与美术效果不在本轮验收范围。
- 证据：`Saved/Logs/P13FinalEditorTests.log`、`P13FinalClientScore.log`、`P13FinalServerScore.log`、`P13ClientHash.log`、`P13ServerHash.log`；`Saved/NetworkVisualVerification/result.json`、`client1_hud.png`、`client2_late_hud.png`、`cook_client.log`、`cook_server.log`。
- 验收脚本新增 `--render`；最后真实图形验证复用刚完成的最新 Cook 包（结果 `fresh_build_cook_stage_package=false` 表示该次不重复 Cook，不是使用旧版二进制）。
- 边界：预置共享地图塔没有玩家归属，HUD 明确显示个人分未归属，不把团队分冒充个人分。实体→玩家归属与个人分、Bot 自动布塔、准备/返回循环由后续阶段补充。Android 与正式美术继续延后。

---

### P14：Bot 与单人完整对局

**目标**：单人（含 Bot）完整打完一局。

**核心任务**：
1. Bot 玩家：确定性 AI 自动布塔/参与，决策可复现。
2. 完整对局循环：准备 → 对局 → 结算 → 返回。
3. 单机自托管（Standalone/Listen Server）。

**验收**：单人（含 Bot）完整打完一局，具备准备/对局/结算流程；Bot 决策可复现。

**建议提交**：`功能：新增确定性 Bot 与单人完整对局循环`

**实施状态（2026-10-11）**：准备阶段确定性布塔 Bot 与 Standalone 完整对局闭环已实现并验收；尚不是独立 AI 玩家或对局中动态决策。
- 新增共享 Bot 准备事务：按槽位 GUID 稳定排序，每塔100资源、最多2塔，检查已占用与重合位置、官方允许标签、ID冲突及内容预算；使用统一 Command 在独立运行候选文档应用，不回写作者准备文档。
- 单人默认场景用两处塔槽替换预置塔，开始时 Bot 自动布塔并消费资源；塔在 PlayAuthority 初始化时注册战斗，未以动态 Spawn 假装具备塔攻击。
- GameMode 新增准备、开始、返回重开接口；同 World 清理旧 Session/Logic/Actor，并复位 GameState 资源、计分冻结、终局通知与阶段。单人来源标记隔离外部签名会话，返回不能误关闭非单人对局。
- 新增 PlayerController：Enter开始、Escape返回，Standalone 俯视相机；HUD 显示准备/成本与结算返回提示。开始和返回只接受本地权威调用，远端 Client 无控制权限。
- 修复客户端跨局 InProgress 快照未重置终局通知，以及 Bot 仅检查原始场景而未检查本轮重合塔位的问题。
- Editor 全量 **109/109** 通过；Client/Server 各1项纯 Bot 事务测试通过，固定输入候选哈希完全一致。两局同 World 重开与作者文档不变已在 Editor/Client 验证；Dedicated Server 禁止本地单人起局，故不伪装成该 target 支持单人流程。
- Win64 Editor/Client/Server Development 构建与完整 Cook/Stage/Package 成功；既有真实 DS+双 Client 晚加入计分复制回归通过。
- 实际 Standalone 渲染对局通过 PlayerController 开始、Bot布2塔、自动三波结算及返回准备；终局团队Score1400，固定输入回归资源90；返回后资源200、无会话、阶段Preparation。已产出 `Saved/SoloVerification/settlement.png`。
- 证据：`Saved/Logs/P14AllEditorGate.log`、`P14FinalClientBot.log`、`P14FinalServerBot.log`、`P14FinalSoloSmoke.log`；`Saved/SoloVerification/result.json`；`Saved/NetworkVisualVerification/cook_client.log`、`cook_server.log`、`result.json`。
- 边界：Bot 当前为准备阶段布塔助手，不包含补位玩家、对局中策略或个人得分归属。Listen Server API 未独立人工验收；房间大厅、2–4人流程留 P15；正式相机/照明美术与 Android 仍延后。

---

### P15：2–4 人联机私测

**目标**：多客户端稳定联机。

**核心任务**：
1. 房间/大厅流程（建房、加入、开始、退出）。
2. 多客户端联机（复用 Phase 0 复制基础）。
3. 断线重连与中途加入（复用 Join-in-progress）。

**验收**：2–4 人稳定联机完成对局；10 个种子作品可稳定联机（门禁）。

**建议提交**：`功能：新增房间大厅与 2–4 人联机私测`

**实施状态（2026-10-11）**：无后端 IPv4 直连大厅及本机多进程联机门禁已实现并通过；公网、平台身份认证与真实跨设备私测尚未执行。
- 新增服务器名册：2–4 人、全员 Ready、仅房主开局；PreLogin 与 PostLogin 双重容量检查，第五人通过 RoomFull 拒绝，并发超员走标准 Kick/连接关闭。
- owning PlayerController 的准备/开始/返回 RPC 从服务器分配身份识别本人，不接受客户端指定其他玩家。Listen 本地主机为房主；DS 首个成员为房主，离开转移下一成员，重连分配新身份且不恢复旧权限。
- 本地 HostRoom 使用 Listen OpenLevel；JoinRoom 仅允许严格规范化 IPv4:port，拒绝 URL、附加 travel 选项、空地址、组播及非法端口；LeaveRoom 带 local=1 明确覆盖命令行房间/签名包选项。Enter 准备、F5 房主开始、F6 建房、Escape 离开或房主结算返回。大厅 HUD 展示名册、准备和房主。
- 房间模式签名包先验签并准备、等待开局；验签失败禁止默认作品回退和登录。公开外部会话初始化拒绝房间模式，单人入口收紧为 Standalone；共享内部初始化仅在通过大厅门禁后执行。
- 计分快照的原子网络序列化增加 RoundId；轮次与结算复制协调后才发终局通知，按轮次去重，避免重开未观察到中间状态或旧轮快照导致错误结算。RoundId 是会话元数据，不纳入可复现玩法终态哈希。
- 最终 Editor 全量 **112/112** 通过；Client/Server 地址与名册门禁各2项通过，标准胜负及1–3波哈希各2项通过；三 target Development 构建、完整 Cook/Stage/Package 成功。
- `verify_rooms.py`：DS+4独立 Client，10种签名参数作品各2轮对局，合计20个四人对局全部完成；每种作品实际拒绝第五人，四客户端最终轮次/分数/资源/生命一致。Listen 验收采用含服务器代码的 Editor -game 主机+3个打包 Client，同样完成2轮。Client-only 包不冒充可建 Listen 房间的 Game 包。
- 另实测2人先开局、运行中加入至4人、RoomFull拒绝第五人、显式离房后新进程重连：成员4被移除后成员5加入，收到当前已结算快照；不宣称恢复原身份或模拟异常断网。三人数量是中途加入路径，未单列三人从大厅开局用例。
- 真实 Standalone 渲染完整循环与既有非房间签名 DS→双客户端视觉/计分回归通过。Listen 多轮测试修复本地同步 RPC 早于阶段投影及结算传播的测试时序问题。
- 证据：`Saved/RoomVerification/ds_result.json`、`listen_result.json`、`reconnect_result.json`及对应每种作品日志；`Saved/Logs/P15AllEditorGate.log`、`P15ClientUnitGate.log`、`P15ServerUnitGate.log`、`P15ClientCapacity.log`、`P15ServerCapacity.log`、`P15ClientHashes.log`、`P15ServerHashes.log`、`P15SoloRegression.log`、`P15LegacyNetworkGate.log`。定向Lint与git diff --check通过。
- 边界：仅受控本机直连/局域网基础，无房间发现、NAT穿透、账号认证、可信重连或Listen主机迁移。未实测异常断网超时、Listen房主退出后客户端自动返回、打包Game target建房按钮及大厅人工视觉操作；当前验证多人玩法与复制，不包含玩家布塔/个人分归属。正式美术、Android与跨设备私测继续延后。

---

### P16：本地发布与分享

**目标**：一键发布 + 他人加载。

**核心任务**：
1. 一键发布（打包 Logic Pack + 生成分享载体）。
2. 分享与加载（他人加载分享包，端到端验签）。
3. 本地作品库（浏览、导入、删除）。

**验收**：发布后可被他人加载游玩；验签拒绝被篡改包。

**建议提交**：`功能：新增本地发布、分享与作品库`

**实施状态（2026-10-11）**：本地签名发布、分享文件与独立接收库游玩闭环已通过；没有引入云服务。
- 共享 `FAkUGCWorkLibrary` 服务：构建、Ed25519签名、自验签，生成不可覆盖的 `.akugc` 分享文件与按ReleaseId命名的本地 `.ugcwork` 记录。接收方必须显式提供独立核对的公钥，不将包内自带公钥自动视为可信。
- Creator 增加独立发布/作品库面板：一键发布当前已应用Document快照，填写分享路径及公钥、显式确认信任后验签导入；浏览、导出分享、复制游玩命令、确认删除本地记录。发布不改变Document、修订号或Undo历史；未应用参数草稿不自动发布。
- 发布身份保存在本机 `Saved/UGC/Publisher/private.key`，后续发布复用，损坏拒绝而非静默换钥。分享与库记录均不包含私钥。私钥目前明文保存，目录须由本机用户保护，不得拷贝给接收方；未实现系统密钥链、加密或密钥恢复。
- 游戏端提供 `ImportWork <分享路径> <独立可信公钥>`、`ListWorks`、`ExportWork`、`DeleteWork`、`PlayWork <发布ID>`；F7浏览、F8重验并准备所选作品，Enter开始、Escape返回。`-WorkLibrary=`可指定独立本地目录，必须是可信目录，不能指向未可信下载/网络共享目录。
- 所有导入有界读取（同一文件句柄，文件上限16MiB）；写盘采用同目录临时文件移动、禁止覆盖。发布分享成功但库提交失败时，仅回滚本次新建分享并报告回滚失败，不删除旧记录。库浏览/导出/游玩重新校验签名、内容哈希、能力、Schema与IR，损坏记录不列为可玩，GUID文件名与包ID必须一致。
- 同ID同内容导入幂等；同ID不同可信签名内容拒绝。删除仅影响库记录，不删编辑项目、分享或当前运行会话。本地 `.ugcwork` 中的信任记录属于本机可信状态，不抵抗能同时改写整个库目录的本机攻击者；签名并非作者实名认证。两文件提交无崩溃恢复日志，不宣称跨文件崩溃原子事务。
- 最终Editor全量 **114/114** 通过；打包Client/Server作品库测试各1项通过，涵盖独立库、持久密钥、未信任/错误密钥/篡改/攻击者重签拒绝、同ID冲突、超限拒绝、发布失败回滚、损坏隔离及安全删除。Creator实际项目快照发布与面板构造回归通过，但按钮点击和人工布局尚未验收。
- Win64 Editor/Client/Server构建及完整Cook/Stage/Package成功。`verify_work_library.py --cook`：正式发布服务生成Creator相同默认塔防；独立打包Client通过自己的库导入，完整通关1400分并返回；第二个新进程重验库并真实渲染，再次1400分通关，产出结算截图。
- 证据：`Saved/WorkLibraryVerification/41bf3749695f4736bec0c758d97f5008/result.json`（完整Cook）及 `352530af6bc745a1b0f7bbe69220e600/result.json`（加强测试完成门禁重跑，含结算截图）；`Saved/Logs/P16FinalEditorGate.log`、`P16ClientLibraryGate.log`、`P16ServerLibraryGate.log`、`P16ClientHashes.log`、`P16ServerHashes.log`。四客户端双轮大厅与既有签名DS→双客户端复制回归通过；标准胜负和1–3波哈希保持一致。定向Lint与git diff --check通过。
- 初次端到端失败因为旧测试Factory故意构造25血/99伤害失败场景；正式发布夹具已切换Creator默认模板，保留旧Factory测试语义，未降低通关断言。正式美术、Android、真实跨设备文件传递、签名作品直接用于房间开局及系统级私钥保护继续待验证。

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

P11 已提交 `b3a2699`；P12 中文改写后的提交为 `9935274`、`38a119e`；P13 中文提交为 `08360f0`。正式美术按用户要求延后，不阻塞后续。

P14 已提交 `276b674`；P15 已提交 `49a0e20`；P16 本地签名发布、独立接收作品库与重启游玩门禁已通过，独立中文详细提交；下一推进项为 **P17（基础AI Copilot）**。异常断网/跨设备私测、私钥系统级保护、个人归属分与独立AI玩家仍待补充，不冒充完成。

每个提交前执行（继承 Phase 0 门禁）：
- AkUGCPlatformEditor/Client/Server Win64 Development 编译。
- 完整 AkUGC 自动化测试。
- Lint + git diff --check。

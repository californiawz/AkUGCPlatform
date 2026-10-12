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

**已完成当前交付范围（2026-10-12，本次提交交付）**：Win64基础Copilot的真实DeepSeek参数/受控Message与Timer建图/只读报错解释、编译与Diff、原子应用及Undo/Redo全部验收通过；真实OS菜单/按钮输入已验证。全新Client/Server打包、同一生成文档签名加载与运行、单人游玩、分享重启复玩及DS双客户端复制回归通过。源码交付范围已核对。真人可用性、跨设备联机、其他平台与代理明确不在本次完成范围，不代表Phase 1整体完成。
- Creator独立Copilot面板接入OpenAI兼容chat/completions HTTP请求；模型与需求必填。端点严格校验，拒绝非法端口、用户信息、控制字符、反斜杠、查询和片段；仅HTTPS或明确localhost/127.0.0.1 HTTP，无配置不生成模拟AI。
- 模型响应可为schemaVersion=1完整参数对象，或严格 `{schemaVersion, parameters, logic}` 组合提案；Logic仅新增Message/Timer（<=32节点、<=64连线），连接只能来自现有GameStart/WaveStart或新节点，目标必须新节点，全部新节点事件可达。不允许Spawn、脚本、替换/删除旧图、任意Document/Command注入。编译器额外拒绝循环、Message出边与Timer多出边。
- JSON原生类型严格校验（数字版本与坐标、字符串id/type/message/引用），拒绝UE隐式转换、未知/重复字段、非法引用、null、超限消息、坐标及波次预算。参数+图组合在副本上一次事务、预算校验及Pack编译，失败不改项目。
- 新增运行时/Editor受控ApplyCopilotProposal，应用前再读原始提案重验，忽略外部Preview里的Transaction或Parameters替换；绑定原始提案SHA-256、Document哈希及Editor修订。旧值/新值及逐节点/连接Diff后显式一次事务应用，Undo/Redo完整恢复，过期或非法组合不改变历史和revision。
- 错误按循环、引用/不可达、终端出边、预算、schema和过期提供本地中文建议，保留原始路径消息，明确不是模型解释。异步取消、关闭项目和销毁面板不修改项目。
- 最新传输已改为项目私有静态libcurl，原生 `FOLLOWLOCATION=0`、`MAXREDIRS=0`，所有选项设置失败均不发送；不修改引擎、不借用HTTP私有handle。仅Win64非XCurl Editor支持，其余平台明确不可用、不回退自动跳转。
- API Key恢复仅HTTPS支持，面板和transport双重禁止HTTP鉴权；Curl显式验证证书链和主机名，生产使用UE证书池。关闭环境代理及netrc隐式身份来源，不记录Authorization内容；测试仅使用无效固定值。
- body写回调在追加前限制256KiB，普通及chunked传输超限中止；刚好262144字节通过、多1字节拒绝。自动解压也在同一回调限额内，限制业务body而非所有网络/TLS/header内存。面板仅在游戏线程Tick收取worker结果。
- worker使用强拥有 `std::thread`，完成请求也先join再删除追踪；模块退出取消全部请求、join全部线程后再cleanup Curl/自身SSL引用。模块化Editor由自己静态Curl初始化SSL副本，单体才配对SSL模块引用；初版误用模块化InitializeSsl返回值导致无请求，已修复并实跑。模块跟踪Tab，退出清空插件内容并关闭；禁止动态模块重载，未宣称支持Live Coding卸载。
- `verify_copilot_http.py` 扩展至31场景：原28种HTTP/TLS/生命周期，增加从真实当前事件ID生成组合图提案成功、循环拒绝与数字消息类型拒绝。固定响应服务是协议夹具，不是模型；HTTP成功图提案确认节点与连线真的应用，再单次Undo恢复。
- TLS用工作区内临时仅localhost证书及仅测试CA参数，不导入系统证书库。服务端确认鉴权仅到达可信TLS端点，302/307与HTTPS→HTTP所有目标均未收到请求；不可信证书与hostname错误必须返回Curl码60且不进入HTTP处理。临时TLS私钥结束后删除，仅保留公共证书及日志。
- 超时配置0.25秒、服务延迟3秒，断言Curl原生码28及1.5秒内完成，最新耗时0.257秒。旧延迟提案777资源、新提案300资源，取消后新结果不被覆盖；新请求pending和预览完成时额外注入旧请求身份完成事件，身份/状态/预览均保留。该注入是门禁单测，不假称服务成功投递旧回调。
- 新增权威运行测试：受控GameStart -> Timer(1秒) -> Message，0.5秒不发消息，1秒边界正确发消息，规则继续运行至胜利。Client/Server独立执行Core提案各2项及Runtime图执行各1项全部通过；组合图只支持安全增量消息/定时链，不冒充任意玩法建图。
- 最新三target Development编译成功；完整Editor **120/120** 通过（31协议场景在其中1项自动化内），包含严格图schema、原子Undo/Redo、摘要/修订过期、注入payload重验与真实Timer执行。定向Lint及git diff --check通过。
- 最终证据：`Saved/CopilotHttpVerification/9dea731a6b794ea9b60631b2293ec57d/result.json`及`editor.log`；`Saved/Logs/P17ControlledGraphFinal_AkUGCPlatformEditor.log`、`P17ControlledGraphFinal_AkUGCPlatformClient.log`、`P17ControlledGraphFinal_AkUGCPlatformServer.log`、`P17ControlledGraphFinalAll.log`；`P17ControlledGraphClientCore.stdout.log`、`P17ControlledGraphClientRuntime.stdout.log`、`P17ControlledGraphServerCore.stdout.log`、`P17ControlledGraphServerRuntime.stdout.log`。所有服务与测试进程已收尾。
- 真实模型验证（2026-10-12，用户明确授权使用已提供Key）：官方 `/models` 查询HTTP 200，模型含 `deepseek-flash`、`deepseek-v4-pro`；通过生产UE证书池及专用禁跳转传输实际执行两次chat/completions，没有测试CA或固定响应服务。Key仅用于进程环境与请求内存，未写入源码、配置和报告；测试结束环境及进程已清理。
- 第一次真实参数请求HTTP 200、耗时1.429秒：两波、每波8个、资源300全部符合；严格参数/文档/预算/Pack编译验证通过，生成不改项目，显式原子应用、单次Undo和Redo精确恢复。成功文档图仍为原2事件、0连线，未意外建图。证据 `Saved/CopilotLiveVerification/758b3126f40740cbb74111fa2ae426a9/parameters.document.json`。
- 第二次真实建图请求HTTP 200、耗时1.679秒，但内部parameters版本不满足约束，原诊断 `parameters.schemaVersion must be 1.`；提案安全拒绝、项目与历史保持不变，未应用。真实报告 `Saved/CopilotLiveVerification/758b3126f40740cbb74111fa2ae426a9/result.json` 记录参数通过、图失败、整体验收false，不能称P17已完成。
- 增加仅 `CopilotLiveAuthorized` 显式授权且有进程Key才运行的真实模型自动化，最多两次生成；无flag全量回归不调用外网。DeepSeek官方默认启用thinking，已对该URL设置 `thinking.type=disabled`；修正纯参数与组合格式提示冲突，并明确内外两层schemaVersion都必须为JSON数字1。内部缺版本回归通过，没有放宽校验或补字段冒充模型输出。提示修正后本轮追加两次授权真实调用，参数与受控图均通过；不放宽校验、不自动补字段或用固定响应代替模型。
- 最新三target Development编译成功，本机31协议场景及 **121项自动化成功**（其中新增真实模型测试因无授权flag跳过，不算额外真实验收）。最新证据 `Saved/CopilotHttpVerification/bde8bf11ab6640f895dd8d0d3891ec50/result.json`、`editor.log`、`Saved/Logs/P17LiveFinal_AkUGCPlatformEditor.log`、`P17LiveFinal_AkUGCPlatformClient.log`、`P17LiveFinal_AkUGCPlatformServer.log`、`P17LiveFinalRegression.log`。定向Lint与git diff --check通过，未提交。
- 本轮真实复测仅两次chat/completions：参数HTTP 200、2.358秒，受控建图HTTP 200、2.397秒；两波每波8个、资源300以及GameStart -> Timer(1秒) -> Message(`Copilot live ready`)精确匹配，验证/原子Apply/Undo/Redo全部通过。真实报告 `Saved/CopilotLiveVerification/cc803178547446ac9e94a6841c8c9506/result.json`；同目录保存 `parameters.document.json`、`logic.document.json`，旧失败报告保留。Key只用于进程环境和请求内存，启动后清理父进程环境，测试进程结束；没有写源码、报告和子进程命令行。
- 新增 `AkUGC.Runtime.Copilot.GeneratedDocumentExecution`：显式输入Saved内文档（<=1MiB、当前schema、单场景），反序列化/预算/编译，再用临时内存私钥签名；序列化回环检查Document哈希，可信公钥验签加载。文档字段与所有原始ID不修改、不稳定化；临时Pack结束删除，私钥不落盘。0.5秒不发消息、1秒一次消息、后续不重复；大步120秒与小步0.25秒至120秒终态一致。
- 同一真实生成文档在Editor/Client/Server独立运行均Victory：16个敌人生成、6击杀、资源360、团队1100分、活跃敌人0；Document哈希和完整终态哈希跨target一致。Client目标中的权威测试World不是联网Client权限，不能称真实多人或跨设备验收。Editor初次与构建并行启动在初始化阶段超时，被终止；构建后单独重试成功，不掩盖失败尝试。
- Runtime证据：真实目录下 `runtime.editor.json`、`runtime.editor.retry.log`；Client/Server报告在Cooked沙箱 `Saved/Cooked/WindowsClient/AkUGCPlatform/Saved/CopilotLiveVerification/cc803178547446ac9e94a6841c8c9506/runtime.client.final.json`、对应WindowsServer的 `runtime.server.final.json`，stdout保存在真实目录。定稿后二者顺序运行避免共享Automation目录冲突，退出码0且实际测试成功。
- `verify_copilot_http.py` 增加 `--generated-document`（必须配合 `--all-tests`，输入位于Saved），全量回归真实复放已有文档并检查Runtime报告，不调用模型。最新 **122项成功**，其中Live模型项无授权flag跳过，GeneratedDocument实际运行；31协议场景仍独立标为固定响应非AI。证据 `Saved/CopilotHttpVerification/a8cfa0a864724d4481916b4f175c8c7f/result.json`、`generated-runtime.json`、`editor.log`；三target编译 `Saved/Logs/P17GeneratedFinal_*.build.log`，全量 `P17GeneratedFinalFullRegression.log`，Lint与diff检查通过。
- 真实测试后补强未来Live验收：文档UTF-8落盘并回读Document哈希纳入通过条件；本轮真实调用发生在该补强之前，已通过独立Runtime回读/签名回环验证实际文件，不追加调用冒充新版Live门禁实测。面板改为逐提案严格验证提示，不再显示已过时的“真实模型尚未验收”。
- 真实参数/受控建图/生产TLS/原始文档权威运行证据保留。本轮新增显式“发送校验诊断并请求模型解释（只读建议）”按钮：只有当前修订存在校验失败、无有效提案/请求才可点击；只发送<=4096字符原诊断，不发送作品、原始需求或API Key内容。不会自动请求解释或执行建议。沿用生产TLS/禁跳转/超时/256KiB响应限额，绑定请求身份和修订，取消及过期丢弃。
- 解释输出严格JSON三字段summary/cause/suggestions，原生字符串类型、非空、各字段长度及1..5建议数约束，拒绝额外/重复字段；与提案解析隔离，永远不产生可应用Preview。保持本地原诊断和建议，并单独标记模型生成只读建议可能不准确、不代表修复；Apply不能使用解释文本，不改变项目、修订或Undo历史。
- 单次授权真实DeepSeek解释HTTP 200、传输码0，含渲染等待约2.944秒：输入错误由实际本地校验空内部parameters产生，诊断 `parameters.schemaVersion must be 1.`。模型返回中文原因，明确缺字段/类型/值均可能且外层未知，建议内外数值版本1、保留其他参数并重新校验。文档/修订/历史未变、不能Apply，证据 `Saved/CopilotExplanationVerification/9c42518336514208ab581688acb84af2/explanation.json`、`editor.log`。本轮只新增1次模型调用。
- 真实可视Slate窗口截图 `Saved/CopilotExplanationVerification/9c42518336514208ab581688acb84af2/explanation.png`，1086x800、120564字节；正常渲染Editor、真实面板及同次模型响应，不是离线重绘。测试仍通过友元调用方法，没有鼠标输入/按钮命中证据，报告 `human_ui_validated=false`；不把截图冒充人工点击验收。窗口与编辑器均收尾。
- 协议扩展至38场景，新增解释成功、数字原因拒绝、额外字段拒绝、项目修订过期、取消、重复字段、单条建议513字符拒绝。夹具硬检查用户消息只含上述本地诊断；固定响应仍非AI。最终三target编译通过、**123项成功**（Live生成和Live解释2项无授权flag跳过；真实文档Runtime实际运行），证据 `Saved/CopilotHttpVerification/0066cb11d6e546ffbe9f66cc6d83734c/result.json`、`editor.log`，`Saved/Logs/P17ExplanationFinal_*.build.log`、`P17ExplanationFinalRegression.log`。Lint、脚本语法和diff检查通过。
- 真实解释调用后只补强请求模式结束清理、chat content原生字符串门禁及重复/超长回归，未追加真实调用冒充定稿全量Live测试。Key仅测试子进程环境传递、启动后清理父环境，没有写源码/配置/报告/子进程命令行；聊天中已暴露，仍建议撤销轮换。
- 本轮新增 `AkUGC.Editor.Copilot.OSMouseInteraction` 与脚本 `--mouse-ui`：正常渲染Editor、真实 `SAkUGCCreatorPanel` 测试窗口，观察器只读状态/布局/截图，不调用New/Generate/Apply/Undo等业务方法。Python ctypes向Windows发送真实鼠标/Unicode键盘输入，先匹配窗口所属Editor进程ID，每次输入核对前台；点击必须由Slate真实命中路径确认目标，仅读输入框Hint不读密钥内容。线程DPI感知并用Windows窗口矩形映射Slate相对坐标，防止多屏缩放偏移。
- OS八检查点按顺序通过：新建默认塔防、输入配置并生成不改文档、点击应用一次事务、Undo精确恢复、Redo精确恢复、取消延迟请求且不修改、非法内部版本提案拒绝且Apply禁用、显式解释且仍只读/Apply禁用。共18次真实命中确认点击；本机POST `/ok`、`/delayed`、`/ui-invalid`、`/explain-ok`各一次，无Authorization。UE独立检查文档哈希与历史并保存8张真实Slate截图，检查点不能重复凑数。没有真实模型请求。
- 最终OS证据 `Saved/CopilotHttpVerification/d6a3ce93fbfb4e1ca2f6ababedff5a46/result.json`、`ui-result.json`、`os-input-actions.json`、`editor.log`、`new.png`/`preview.png`/`applied.png`/`undo.png`/`redo.png`/`cancel.png`/`rejected.png`/`explained.png`。固定响应仅用于UI协议验收，不能冒充AI生成。Creator是在专属测试窗口呈现，不包含Window菜单打开入口；`human_ui_validated=false`准确表示无人真人测试，OS输入流程已验证。
- 前5次OS驱动尝试失败证据保留：标题遮挡/未展开、文件共享PermissionError、DPI坐标偏移、输入框命中标签为空等，均及时终止测试进程；未把失败归为业务通过。修正后第6次六阶段通过，定稿第7次八阶段通过。正常取消的保活ConnectionAbortedError只在夹具handle层忽略，不吞其他服务错误。
- 定稿三target编译成功；全量 **124项成功**（Live生成、Live解释、OS输入3项因无显式参数跳过；GeneratedDocument实际执行），38协议场景全部完成，原始真实生成文档Runtime仍1100分Victory。证据 `Saved/CopilotHttpVerification/781b2e3d29b34a3089137a7cac62185d/result.json`、`editor.log`，`Saved/Logs/P17MouseUIFinalBuild.log`、`P17MouseUIFinal_AkUGCPlatformClient.build.log`、对应Server日志、`P17MouseUIFinalRegression.log`、`P17MouseUIFinalRun.log`。Lint/py_compile/diff检查通过，无残留Editor/夹具进程，本轮未调用真实模型。
- 本轮扩展OS观察器 `CopilotMenuEntry`：从真实 `FGlobalTabmanager` 根窗口开始，只遍历真实主窗/子窗口/弹出菜单，Python实际点击“Window/窗口”与“AkUGC Creator Studio”，独立确认注册Tab与Copilot子面板存在，没有直接TryInvokeTab替代入口点击。菜单模式与真实生成模式独立运行，菜单零网络请求。证据 `Saved/CopilotHttpVerification/92bdfcd81dab45a1a6fe2475393aa3f4/result.json`、`menu.png`、`os-input-actions.json`、`editor.log`。
- 本轮单次真实模型+OS点击串联：专属真实Creator窗口，OS填写官方HTTPS URL、deepseek-flash、密码框与中文两波/每波8/资源300需求，只点击生成一次；HTTP200、严格参数精确匹配，预览不改项目、点击Apply一次事务、Undo与Redo文档哈希精确恢复，5检查点及12次真实命中点击通过。Editor不继承Key环境，Key仅Python环境临时读出并经密码框OS输入，动作日志仅标签/长度，所有文本输出无真实Key。证据 `Saved/CopilotHttpVerification/d2e404bf5f1e424080b25eb70801d812/result.json`、`preview.png`、`applied.png`、`undo.png`、`redo.png`、`os-input-actions.json`、`editor.log`，零本机夹具请求。本轮只新增1次真实生成。
- 首次真实OS尝试 `Saved/CopilotHttpVerification/d46b4c7190354797a61bd84997db3e30` 在新建前失去前台被安全停止，HTTP0/无pending，未输入Key、未发模型请求；下一次成功，不掩盖失败。输入只允许本轮Editor进程，菜单弹窗同进程允许，其他应用立即停止。
- 真实点击后发现驱动fill空文本只Ctrl+A未删除：成功运行期间密码仍是掩码，直到面板/Editor退出清理，不能说生成后已即时清空。已修复Ctrl+A后显式Backspace，并只报告ApiKeyEmpty布尔门禁；未来Live截图前及所有UI结束必须确认密码为空。修正后仅用无效fixture-not-a-real-key本机OS填入/清空验证，无模型请求，8流程通过。证据 `Saved/CopilotHttpVerification/5fe779ec4aab453888136a02ab6736da/result.json`，`api_key_empty=true`。没有追加真实生成冒充新版清空门禁实测，旧真实报告保留。
- 最新三target编译成功，全量124项成功（3项显式Live/OS测试跳过），38协议场景及原始真实图文档Runtime实际运行且1100分胜利。证据 `Saved/CopilotHttpVerification/3ba302d06e674b1a9d932a4d1cac74de/result.json`、`editor.log`；`Saved/Logs/P17MenuLiveFinalBuild.log`、`P17MenuLiveFinal_AkUGCPlatformClient.build.log`、对应Server日志、`P17MenuLiveFinalRegression.log`、`P17MenuEntryRun.log`、`P17MouseLiveRun2.log`、`P17PasswordClearUIRun.log`。Lint/py_compile/diff通过，Editor/夹具进程及环境Key已清理，未提交。
- P17按当前Win64受控Message/Timer+塔防参数及只读解释范围，核心技术链已闭环；不保证任意模型输出正确，不支持任意玩法脚本、自动修复执行、代理或其他平台。菜单与真实模型点击分别实跑，不假称从菜单起一整次端到端多场景。真人可用性、跨设备联机与Phase1总体门禁仍待验证；本轮不继续追加模型请求、不擅自提交。
- 收尾只读审查确认并修复3项P2：模型解释原样转发含任意字段名的诊断（可能回显原需求）；chat/completions外层重复choices/content可被后值覆盖；生成配置失败/传输未启动时提前清空旧预览。诊断外送现仅精确白名单固定schema错误或固定类别文字，未知字段名、重复字段名及其他原错误内容不外送，本地原诊断继续保留；界面同时显示实际外送脱敏诊断。外层响应先拒绝重复字段后再解析，不限制正常API额外字段。
- Generate以局部CandidateDocument/CandidateRevision准备请求，Start成功才更新持久上下文并清空旧预览/解释；空需求、非法URL和关闭传输未发送时保留完整ProposalJson、Diff、验证标记、原请求Document哈希/修订、诊断及模型建议。Explain启动失败也保留旧建议。成功新请求及显式Cancel仍按原语义清空，未放宽修订与身份门禁。
- 协议扩展至42场景：重复choices、重复content（最后合法值）均拒绝；虚构PRIVATE_FIXTURE_MARKER作为未知/重复字段产生真实本地错误，标记本地保留但服务端确认解释全消息不含标记。合法预览及已收到解释分别覆盖3类未发送保留，并确认旧提案仍可Apply/单Undo恢复。复核确认测试能针对旧实现失败，但没有实际切回旧实现或改写历史验证，不假称做过mutation实跑。
- 最新三target编译成功，全量124项成功（3项显式Live/OS测试跳过），42协议场景全部完成，已生成原始文档Runtime实际运行仍1100分胜利。证据 `Saved/CopilotHttpVerification/b1ab7ecb6f414d97aa1e832fd9b8b3c1/result.json`、`editor.log`；OS八检查点和密码清空复验通过 `Saved/CopilotHttpVerification/137d91c2cdb84b849a52e0eb656c7641/result.json`，本机4请求无鉴权。构建/运行日志 `Saved/Logs/P17ReviewFixEditorBuild.log`、`P17ReviewFix_AkUGCPlatformClient.build.log`、对应Server日志、`P17ReviewFixRegression.log`、`P17ReviewFixMouseUI.log`。定向Lint/py_compile/diff检查通过，审查后只读复核未发现限定范围内新的阻断问题。
- 本轮没有追加真实模型请求，不把脱敏后泛化诊断的模型质量当作已实测；既有精确内部版本诊断仍白名单保留不变。Editor/夹具进程与临时环境均已收尾，未提交。
- 最终源码交付核对完成：22个P17文件（11个已跟踪修改、11个新增源码/测试脚本），依赖引擎已有libcurl静态库及其OpenSSL/zlib/nghttp2传递依赖，不需复制引擎binary。排除未跟踪Build/WindowsServer文件顺序日志、Content/Lua/Main.luac、GenerateProjectFiles.bat与Python缓存；不删除用户文件，不stage或commit。最后两轮限定范围只读复核未发现额外运行/打包阻断。
- 全新BuildCookRun Client和Server全部完成Build/Cook/Stage/Package、ExitCode0；证据 `Saved/WorkLibraryVerification/f50c5d4d83e54866b5b7f71c44a44d64/cook_client.log`、`cook_server.log`。可运行交付位于 `Saved/StagedBuilds/WindowsClient/AkUGCPlatform/Binaries/Win64/AkUGCPlatformClient.exe` 和对应WindowsServer；其Content/Paks存在。基础Copilot仅Editor提供，打包Runtime不带模型Key。
- 打包独立接收作品库未信任/篡改拒绝、验签导入、完整对局及新进程重启重验复玩通过，两次1400分胜利并返回。证据上述WorkLibraryVerification目录 `result.json`、`play_0.log`、`play_1.log`、`settlement.png`。这是默认模板分享回归，不冒充模型生成分享作品。
- 同一真实生成图文档在新打包Client/Server逐字节复制进各自Saved/P17Delivery再Build/Sign/加载执行；文档字段/ID完全未改，两目标受控图与生成文档Runtime各2项成功，Core严格提案各2项成功；最终1100分、资源360、16刷怪、6击杀、消息边界与大小时间步及跨目标终态哈希一致。证据WorkLibraryVerification目录 `generated.client.final.json`、`generated.server.final.json`、`generated.*.retry.stdout.log`、`core.*.stdout.log`。首次使用项目根Saved被打包目标Saved门禁拒绝（实际Fail但进程码0），失败日志保留；未放宽门禁，后续依报告与测试成功数断言通过。打包时额外观察到用户未跟踪Lua入口返回表错误日志，不属P17交付源码、未擅改；其未阻断本轮对局或Copilot测试，不宣称Lua功能验收。
- 新打包DS+两个渲染Client联网复制/晚加入回归通过，晚加入23.04秒，三端终局统一500分、资源321、基地993、0击杀、Settled1，移动与销毁均观察到；证据 `Saved/NetworkVisualVerification/result.json`、双Client HUD截图、server/client日志。脚本 `fresh_build_cook_stage_package=false`是本次未在该脚本重复cook；所用stage由刚才WorkLibrary脚本全新cook生成，两个事实分别保留。联网夹具非真实模型作品，且同机多进程非跨设备/真人私测。
- 新打包单人Bot准备/开始/完整对局/返回及渲染结算通过，1400分，证据 `Saved/SoloVerification/result.json`、`playable.log`、`settlement.png`。最终Editor全量124项成功（3显式Live/OS项跳过）、42协议全完成、原始真实文档Runtime实际运行，证据 `Saved/CopilotHttpVerification/5acfaade81c648e3b7a93d9ecd67a660/result.json`、`editor.log`。脚本py_compile、修改文件Lint与git diff --check通过，进程已全部退出。
- 完成判定：P17当前约定Win64基础受控范围的实现、真实模型验收、安全审查、UI技术验收、打包与主链回归已完成。未完成项只列为范围外/后续产品门禁：真人可用性、跨设备联机、其他平台/代理、任意玩法建图与自动执行修复、Phase1总体验收。Git提交不是实现完成证据，本轮未明确收到提交指令，不擅自commit。

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

P14 已提交 `276b674`；P15 已提交 `49a0e20`；P16 已提交 `6fd3282`；**P17（基础AI Copilot）当前Win64受控交付范围已完成，按用户授权在本次提交交付**。22文件源码与最终交付快照已核对；真实模型、UI、全量124项（3显式项在全量中跳过）、42协议及全新打包门禁通过，详情见P17记录与本地 `P17.delivery.json`。

后续优先顺序（不计入本次P17完成）：
1. **Phase1真人门禁**：非程序员60分钟内创作并分享首作；至少10个种子作品、2–4人跨设备联机，重点异常断网、重连及计分/胜负一致性。同机自动化不能代替此项。
2. **发布密钥保护**：本机签名私钥系统级保护、文件权限及备份/轮换；聊天中暴露的模型API Key应撤销换新，不能继续作为正式配置。
3. **已延后产品项**：正式美术、Android真机、个人归属分和独立AI玩家按产品优先级推进；其他平台/代理以及更广泛Copilot图能力需另定范围与验收。
4. **已知独立问题**：未跟踪Lua入口返回表错误不属P17且未阻断本轮对局，若要启用Lua功能需单独处理；脱敏泛化诊断质量及菜单到真实模型同会话可作为后续补充测试。

以上未完成，不能将P17技术交付等同于Phase1整体完成。

### 2026-10-12：Phase 1 后续收敛实测

本轮按上下文将用户所说的「P1」理解为 Phase 1 整体，不是 Phase 0 的路径分项 P1。实际仓库 HEAD 为 `90b0be5`（P17 本地提交）；先前会话关于未提交的更正不符合当前仓库事实。本轮后续修改未提交、未推送。

**已完成本轮子任务：**
- 新增独立参数 OS 输入模式 `--mouse-ui --parameter-ui`，真实数值键盘输入基地1700、资源425、两波；草稿不改文档、Save不保存未应用草稿、一次Apply、Undo/Redo、Save/Load、放弃草稿共9检查点通过。保存仅在显式开发测试模式隔离到本次Saved目录，不覆盖原默认项目。零模型/HTTP请求；自动输入不等于真人验收。证据 `Saved/CopilotHttpVerification/36e4e089a97a4ee098bd8961e39f78a0/result.json`、`os-input-actions.json`及9张截图。
- 真实复现Listen房主离房后客户端保留`-AkUGCRoom`，自动默认地图返回又创建单成员房间、无法进入干净单人准备。修复`InitGame`：只在Standalone非Dedicated世界将`local=1`、`closed`、`failed`作为本地隔离；Listen/Dedicated不能使用此覆盖跳过房间及签名包初始化。新增NextURL派生Listen与Standalone对照断言，全量回归实际执行通过。原复现失败保留 `Saved/RoomVerification/5a5fefc201764ade86ec5aa8fea9d1ea/receiver.log`。
- 新增独立故障恢复门禁：实际双方成员身份与名册就绪后握手，再注入主动离房、双向100%静默丢包、强制终止本轮主机进程。恢复必须无NetDriver、成员/名册清空、非房间、无旧会话、Preparation，并实际单人开始及返回。最终脚本要求房主正式Automation成功、接收进程退出码0且退出后双日志无Fail；每轮UUID目录、空闲UDP端口及实际IpNetDriver监听端口核对。三模式定稿重跑均通过，证据分别 `Saved/RoomVerification/761856908a084a97a7b42bdcb39ede6f/recovery_result.json`（leave）、`8cc058834b294385a1d6fbaecc2a5af5/recovery_result.json`（timeout）、`7b055ecceecf40c188e659ff558216d1/recovery_result.json`（主机强制终止）。静默丢包测试超时显式4秒，主机终止保留默认60秒原生ConnectionTimeout。均为本机Editor -game，不是假称新打包Game或跨设备故障验收。
- 修复参数波数变化在鼠标捕获期间重建整个控件的潜在析构回滚：延迟重建等鼠标捕获释放。修复已编译，但当前系统的OS合成鼠标移动未能驱动SpinBox高精度raw-input数值变化，滑动实测仍失败；显式`--slider-ui`保留严格失败门禁。普通参数成功报告明确`numeric_slider_verified=false`，不把键盘验收当作滑动通过。
- Editor/Client/Server Development构建均成功；Editor全量124项框架Success（真实生成、真实解释、OS点击3项在全量中明确跳过），42协议场景实际全部执行；原始真实生成图文档签名加载、消息1秒边界、大小步终态仍一致，Victory1100分。证据 `Saved/CopilotHttpVerification/f9d586c217624f939580b3c70aab84aa/result.json`、`generated-runtime.json`，`Saved/Logs/Phase1FinalEditorBuild.log`、`Phase1FinalClientBuild.log`、`Phase1FinalServerBuild.log`。故障测试为独立显式ClientContext用例，不计入Editor全量通过数。
- 全新Client/Server Build/Cook/Stage/Package各ExitCode0；10个签名种子、4独立Client各2轮，总计20局全部一致结算，每种作品均拒绝第五人。证据 `Saved/RoomVerification/096cca6a96ef43a299fa5607b74bf601/ds_result.json`及同目录完整cook日志与逐种子日志。此处NullRHI只验证玩法/网络对象，不代表视觉验收。
- 复用上述本轮全新包继续通过Listen四人双轮、晚加入及显式离房后新进程再进、独立作品库验签导入/未信任与篡改拒绝/重启两次1400分复玩。证据 `Saved/RoomVerification/6d9a02c44d2c46899a505d85f363a24f/listen_result.json`、`9d3c6e07e7784d068b4c028cf18e2e8b/reconnect_result.json`、`Saved/WorkLibraryVerification/7f579beb19044be6a33fe4e75a7f4810/result.json`及实际渲染`settlement.png`。这些脚本的`fresh_cook=false`仅表示未重复打包，所用stage由本轮十种子脚本新cook生成。
- 两轮限定范围只读审查后修复服务器覆盖边界、恢复端口串场和最终双日志完成门禁。定向Lint、Python语法、git diff --check通过；测试进程均收尾，本轮无真实模型调用；未改用户未跟踪Lua、Build文件或项目脚本。

**仍不能判定 Phase 1 完成：**
1. 真人非程序员60分钟首作、跨设备2–4人十种子私测及真实文件传递，当前缺外部设备和参与者，不能由本机自动化代签。
2. 参数滑动实测仍未通过；下述作品库续轮已补齐发布/导出/取消与确认删除/验签导入完整OS点击链，自动输入不等于真人核钥或首作验收。
3. 下述最新续轮已补齐双端OS键盘JoinRoom、准备/开局/两轮胜利/返回流程，但截图HUD外三维场景仍黑，三维视觉验收未通过；不能把输入与状态成功当作完整视觉切片交付。
4. 正式美术及Android按已有用户约定继续延后；发布私钥系统级保护仍待推进。P17完成不受此重复撤回，但Phase 1整体Go/No-Go保持未通过。

所有 `Saved/` 证据为本机实际文件且被gitignore忽略，不是随源码提交的永久归档。失败尝试保留；初始化超时、早于名册就绪的房主退出、预期互斥网络错误误配置、Trace控制端口误匹配、abslog退出前尾部刷新竞争及raw-input滑动失败未被删除或计为成功。

### 2026-10-12 续轮：独立 Game 入口与包级恢复

- `verify_rooms.py`新增显式`--packaged-recovery`及`--host-entry-ui`。前者实际使用完整stage内部Game/Client与Pak，后者仅允许包级leave用例。接收端`UserDir`隔离在本轮Saved，并保留握手只能写入真实ProjectSavedDir的C++限制；目标日志必须确认Editor=0及Client/Game编译身份。
- Game和Client完整Build/Cook/Stage/Package均ExitCode0。最终Cook通过Windows Job Object封装：子命令启动前先完成Job绑定，超时/异常关闭Job清理整棵后代进程；实测成功、非零拒绝、超时拒绝及孙进程终止通过，证据`Saved/RoomVerification/6cf23b2e2fb44f238f37548833367014/job_cleanup_result.json`。第一次CLI引号构造失败未产生成功结果，保留原会话错误，不计通过。
- 同一完整Game包从无`listen`、`room=1`、`-AkUGCRoom`的默认单人准备启动，核对窗口PID/前台，真实OS点击viewport并按F6；随后实际IpNetDriver绑定本轮端口、本地房主名册出现，接入独立打包Client。保存准备→房间→恢复本地准备三张真实窗口截图与OS输入记录。最终证据`Saved/RoomVerification/6c5acc76616f44b1813b145f7fbc3c6f/recovery_result.json`、`os-input-actions.json`及`game-local-before-f6.png`、`game-room-after-f6.png`、`game-recovered-local.png`。自动输入非真人验收；Client在本次直接启动连接URL，未声称验证可视JoinRoom入口。
- 最终包级leave、双向100%丢包timeout、强制终止Game主机crash三模式均通过：Client目标实际为Editor=0/Client=1；无NetDriver/名册/旧会话的本地准备与单人开始/返回断言、正式Automation Success、接收进程退出码0、最终双日志无Fail及非故意crash场景主机仍存活共同成立。timeout显式4秒，crash保留原生60秒。证据分别为上述`6c5acc...`、`Saved/RoomVerification/c77b1aa5eec84fd68598fa76e4505859/recovery_result.json`、`7b7808f4570942db843a1771625af859/recovery_result.json`。全是同机，不代表跨设备或运行中故障验收。
- 修正Client HUD：Client编译条件不支持Listen建房，不再显示不可用的F6建房提示；Game/Editor保留F6文案，与`HostRoom`编译条件一致。
- 最终Editor/Server构建成功；Game/Client由最终完整Cook构建成功；Editor全量仍为124项框架Success、42协议场景实际执行，3个显式真实模型/OS项在全量内明确跳过。原真实生成文档签名运行仍1100分Victory，大小步一致；本轮零模型请求。最终报告`Saved/CopilotHttpVerification/fe721015e975458c97fa0103f7e73e05/result.json`。限定审查发现并修复Host旧成功日志掩盖提前退出、Cook超时只结束父进程两项异常门禁。

**本续轮完成的只是上述子任务；Phase 1整体仍未通过**：参数滑动、发布/导入/导出/删除确认完整OS链、完整可视房间回合流程以及真人/跨设备门禁继续待办。本轮不提交、不推送，不新增模型请求；现有其他延后项保持原约定。

### 2026-10-12 续轮：作品库完整 OS 点击链

- 用现有`OSMouseInteraction`扩展互斥`--mouse-ui --library-ui`；实际`-UserDir`将ProjectSavedDir、Library、Publisher、Share和默认项目全部隔离于本轮UUID目录，面板构造与OS新建输入前均严格断言路径。未改签名协议、库格式、核心DefaultDirectory语义或用户现有发布私钥。
- 将同步模态删除对话框改为面板内非阻塞等待的确认区，显示目标名称与发布ID；待确认时所有作品行禁用，取消不写库，确认只删除本地记录。请求及确认仍拒绝纯无人值守输入；引擎`IsUnattended()`包含`GIsAutomationTesting`，故本次OS测试显式`-UnattendedInput`但不自动点击确认。新增`UnattendedDeleteGuard`在全量中实际执行，验证无交互许可时请求/确认均不推进删除、取消清待确认状态（使用未构建面板和随机ID，无真实作品文件）。
- 11个有序检查点及11张截图通过：新建、Save、发布、导出、待删除、点击取消、再次请求后确认删除、未信任禁用导入、错误公钥明确验签拒绝、正确导入、重复导入成功且幂等。实际鼠标/键盘事件均核对本次PID与前台，点击可启用控件前要求真实Slate命中；禁用控件只读有效父级启用状态，不强行点击。最终证据`Saved/CopilotHttpVerification/4a098ec3ba96498eb663737e6e0e10a3/editor-user/Saved/verification/result.json`、`os-input-actions.json`及11张截图。
- 全程当前文档哈希、修订、Undo/Redo及已保存文档不变；导出与发布作品分别验签后语义序列化一致；删除/重导入保留两个分享文件自身SHA256不变；最后一条同发布ID作品有效。错误公钥检查实际输入路径/键及同文件验签诊断，重复导入必须当次显示明确成功，不能靠旧记录有效假通过。发布前实际勾选旧信任，发布后断言清除。发布者公钥来自单独发布状态，不从待导入包自动信任；报告明确真人独立核钥渠道未验证。
- 修复长信任确认文案不换行、滚动缓存几何不代表实际裁剪可命中、Windows原子检查点替换被短暂读取锁占用（同一序号3秒内有限重试，超时仍失败）等实测问题。失败日志均保留，包括后台3FPS启动、Automation被视为unattended、禁用控件不能命中、路径分隔符文本比较、短暂读锁竞争；未删除失败记录或计为通过。
- Editor/Client/Server Development构建成功，Client/Server本轮无源码影响且up-to-date；全量125项框架Success（3显式真实生成/解释/OS项全量内明确跳过）、42协议实际执行，新增无人值守门禁实际执行。既有真实生成图签名运行仍1100分Victory，大小步一致，报告`Saved/CopilotHttpVerification/3e1107393c7f4e5690ae111b382a8245/result.json`。另独立串行实跑参数9点、Copilot夹具8点均通过，报告分别`e8d0a780f1c94e2a8ed60130eed3726c/result.json`、`a53a2e90f2ed4f13978bab6299694b20/result.json`（同Saved/CopilotHttpVerification根）。
- 本轮零真实模型请求；最终库用例HTTP请求为0。实际核验分享、作品库记录和结果/输入报告不包含本次生成的发布私钥；私钥仅存本轮隔离Publisher/private.key，未在截图或报告输出。过程记录均本机Saved，未提交、未推送。Phase 1仍未通过：参数滑动、完整可视房间两轮、真人60分钟首作与跨设备十种子门禁继续待办。

### 2026-10-12 续轮：双端键盘两轮与滑动阻断定位

- 新增显式`verify_rooms.py --keyboard-rounds`，双端完整Game/Client均真实渲染窗口，从无room/listen启动的单人准备开始，通过OS键盘F6建房、控制台输入JoinRoom、Enter准备、F5开局、Escape返回执行两轮。只读C++观察器不调用Prepare/Start/Return，不修改作品，仅写本轮隔离Saved中的状态和检查点。
- 单成员F5、双方未准备F5、双方已准备时非房主F5均实际拒绝；房主才能开局。两个目标各12个有序成功检查点、共24张截图；两轮最终胜利快照逐字段严格跨端相同，分别Round2和Round4，1400分、9击杀、资源90、基地790/1000、活跃敌人0；每次Escape后分数/结算清零、准备重置且回合号增加。最终比较使用各端C++确认的settled检查点，不使用此前另读state假称一致。
- 最终Game/Client完整Build/Cook/Stage/Package成功证据位于`Saved/RoomVerification/f6ea6b20d9a748ba80c434aaf64e4cac/cook_game.log`及`cook_client.log`（均ExitCode0）。该轮实测初期Job设置类别错误87而未启动对局；修正为ExtendedLimits后复用其最终完整包，成功报告`Saved/RoomVerification/0db465621a184146a14d2423d3cba062/keyboard_result.json`、`os-input-actions.json`及各端Saved/keyboard/result.json和24截图。`fresh_cook=false`准确表示成功重跑未再次cook，不代表使用旧源码包。首次成功双轮报告`f850d38b935e4b178abed7b68a0dd52c/keyboard_result.json`保留，早期失败`b8e705...`为JoinRoom travel已关闭控制台却再次盲切Tilde打开造成Enter准备不生效；驱动改为读取实际console_open状态。
- 完整两轮运行进程绑定Kill-on-close Job；最终两个Automation正式成功且实例仍存活才输出结果，关闭Job后逐进程收尾、日志流关闭，清理失败删除成功结果。拒绝复用旧成功目录。新版OSKeyboardTwoRounds缺RoomKeyboardOutput参数明确失败，不把未执行记Success；实际缺驱动负向门禁在`Saved/RoomVerification/f89d5deb197d47939ed73fc5c3a350b6`记录Fail通过预期。
- **重要视觉边界**：两端HUD和交互截图可读，但HUD外区域实际RGB全0，三维场景黑背景问题仍未修复。相机保持、默认Pawn/possession和光照路径只读检查尚未确定唯一原因；不能把两轮状态及HUD通过宣称三维可玩视觉完成。驱动结果明确`scene_visual_validated=false`；定稿带此边界的实际重跑同样两轮成功，报告`Saved/RoomVerification/6d01ee7d03ad458f9f27965cf6efca1a/keyboard_result.json`及24截图。已有`0db465...`成功证据由本段补充解释、不伪造更新历史文件。
- 参数滑动阻断已定位到当前引擎Windows高精度分支：WM_MOUSEMOVE仅在非高精度时路由；ForceRawInputSimulation处理的是绝对RAWMOUSE，不会让legacy合成移动变成可用raw输入。OS按下可捕获并不证明SpinBox收到非零raw delta。未改引擎、未伪造WM_INPUT、未注入Slate或调用参数回调冒充通过；真实HID/正规虚拟HID输入条件当前未具备，保留滑动未通过和原失败证据。
- Editor/Server构建通过，Game/Client在上述完整Cook中构建；全量仍125项框架Success（3显式真实生成/解释/OS Editor项明确跳过），42协议实际执行，原生成图签名运行仍1100分Victory且大小步一致；最终报告`Saved/CopilotHttpVerification/2d39b3b783424bc7927626cb73d3a3a4/result.json`。新键盘用例为独立ClientContext，不计Editor全量。

本轮完成的是键盘两轮子任务；**Phase1仍未完成**，剩余三维黑背景视觉修复、真实HID滑动验收、真人60分钟首作/跨设备十种子私测。正式美术与Android继续按约定延后，私钥系统级保护仍后续。未提交、未推送、未追加真实模型请求，测试进程已收尾。

### 2026-10-12：按用户要求提交当前阶段成果

本次提交包含已验证的断线恢复、参数与作品库 OS 交互、双端键盘两轮观察器、测试驱动及本记录，共12个文件；不代表 Phase 1 整体完成。此前各续轮的“未提交”描述是当时状态，本次按用户要求进行本地阶段提交，不推送远端。

提交前重新核对既有125项框架成功、42协议、作品库11点与键盘两轮结果，并检查当前脚本AST及git diff --check；本次未重跑构建或全量测试。期间新增的 AkUGCPlatform.uproject、外部插件/脚本及其他未跟踪文件不纳入本阶段提交，也不作为上述验收结果的一部分。Saved证据仍仅保存在本机，不提交私钥、缓存、日志或截图。

每个提交前执行（继承 Phase 0 门禁）：
- AkUGCPlatformEditor/Client/Server Win64 Development 编译。
- 完整 AkUGC 自动化测试。
- Lint + git diff --check。

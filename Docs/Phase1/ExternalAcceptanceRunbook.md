# Phase 1 外部验收执行手册

> 适用范围：Phase 1 Win64 技术候选版。
> 本手册用于真人、实体鼠标和跨设备门禁；同机多进程、OS 合成输入及自动化报告不能替代这些门禁。

## 1. 判定边界

外部验收由三部分组成，缺一项即保持 No-Go：

1. 实体鼠标完成 Creator 波数滑动，并由旁观者见证。
2. 一名非程序员在 60 分钟内，仅使用 L1 参数和少量场景摆放完成首作、保存、发布、导出、传递，并由另一设备验签导入和游玩；不得使用 Logic Graph。
3. 至少 10 个不同签名种子作品在真实网络和不同物理设备上完成 2–4 人联机；样本必须同时覆盖 2 人与 4 人，结果、团队分、基地生命和资源在各端一致，并完成离房、断网、主机终止及重连恢复。

`phase1_external_acceptance.py`只校验证据文件、摘要、时间、跨端数值一致性和显式声明。它不能独立证明测试者背景、实体 HID 来源、设备确实不同、网络条件或人工陈述真实，因此结果会固定保留：

```json
"software_independently_verified_human_claims": false
```

## 2. 冻结同一候选版本

先在验收机安装 CPython 3.14 x64，建立独立环境并做预检；后续命令均使用该环境。`$EngineRoot` 改为本机候选版对应的 Unreal Engine 根目录：

```powershell
$EngineRoot = "E:\AkStudio\UnrealEngine"
py -3.14 -m venv .venv-phase1-acceptance
$Python = ".\.venv-phase1-acceptance\Scripts\python.exe"
& $Python -m pip install `
  -r Tools/Tests/requirements.txt
& $Python -m unittest discover `
  -s Tools/Tests -p "test_*.py"
```

1. 为 Creator、Listen Game、Client 和 Dedicated Server 生成最终候选构建。测试中途不得替换任何一项。
2. 将完整 Creator 候选目录（项目 Content/Config/Lua、实际插件模块及引擎 BuildId 清单）、Game、Client 和 Server 分别压缩为归档文件。归档中不得包含旧版 `private.key`、DPAPI 保护的 `publisher-identity.json`、模型 API Key、Publisher 私钥目录或个人配置；密钥已加密不代表可以收集或分发。
3. Creator 标识文件必须使用本轮实际加载的 `Plugins/AkUGCEditor/Binaries/Win64/UnrealEditor-AkUGCEditor.dll`；人工滑块报告会重新计算该文件摘要。
4. 在项目 `Saved` 下建立全新的会话目录，将 Creator 完整归档、Creator 模块标识和三个运行包共五个文件复制到该目录的 `baseline/`；初始化工具拒绝引用目录外文件、空文件、`.key` 或符号链接。
5. 提交候选源码并确认工作区完全干净，再初始化清单。工具会拒绝 `--source-commit` 与当前 HEAD 不同、存在已跟踪修改，或存在任何未被 `.gitignore` 排除的未跟踪文件；五个文件必须同时提供，工具会保存相对路径、随机会话 nonce 和 SHA-256，最终校验时重新读取文件核对：

```powershell
$Commit = git rev-parse HEAD
& $Python Tools/Tests/phase1_external_acceptance.py init `
  --output Saved/Phase1ExternalAcceptance/<session>/acceptance.json `
  --source-commit $Commit `
  --creator-build Saved/Phase1ExternalAcceptance/<session>/baseline/CreatorBuild.dll `
  --creator-package Saved/Phase1ExternalAcceptance/<session>/baseline/CreatorPackage.zip `
  --game-package Saved/Phase1ExternalAcceptance/<session>/baseline/GamePackage.zip `
  --client-package Saved/Phase1ExternalAcceptance/<session>/baseline/ClientPackage.zip `
  --server-package Saved/Phase1ExternalAcceptance/<session>/baseline/ServerPackage.zip
```

初始化时会确认四个归档均为有效 ZIP 且不含私钥文件。Creator 归档必须包含启用 AkUGCEditor 的项目描述、Content/Config、Editor 插件描述、非零 `UnrealEditor.modules` BuildId 及唯一有效 Win64 AkUGCEditor PE DLL，且归档内 DLL 摘要与独立模块标识一致。Game、Client、Server 归档必须分别包含有效 Win64 PE、与角色匹配且 BuildId 非零的 `.target`、项目描述及带 Unreal Pak 尾部标识的 Paks。结构检查与归档 SHA-256 使用同一打开文件流，并要求检查前后摘要一致。清单初始化后不得覆盖重建。工具会预填 10 个 `seed_runs` 和四类 `resilience_runs` 的完整字段骨架；Listen 玩家角色和每类恢复的 host/client 报告也已按模式预填，空值是待执行占位符，不代表已通过。所有证据路径必须相对 `acceptance.json` 所在目录，且文件必须保留到最终封存和校验完成。测试者、观察者和最终协调者使用别名，三类角色必须相互独立；不得复用同一路径或同一内容充当不同证据。

这些是静态格式、角色清单和摘要绑定检查，不能单独证明二进制可启动、BuildId 与指定引擎实际兼容或 Pak 可被产品完整加载。Creator 的真实模块加载由人工滑块运行现场核对，Game/Client/Server 与 Paks 则必须由后续候选包启动、十种子和故障恢复原始证据补足；不得把初始化成功写成候选已可运行。

### 2.1 可选 clean HEAD 本机全新构建

日常工作区有未提交改动时，先从候选提交建立全新的 detached worktree。该 worktree 中不得已有 `Binaries`、`Intermediate`、`Saved`、`DerivedDataCache` 或 `Content/Lua`。构建来源报告必须写到 worktree 之外的全新目录：

```powershell
$Commit = git rev-parse HEAD
& $Python Tools/Tests/build_phase1_candidate.py `
  --project-root . `
  --source-commit $Commit `
  --engine $EngineRoot `
  --luac E:/AkStudio/UnrealGameBuilerTool/LuaTool/Win64/luac.exe `
  --output <build-root>/Saved/Phase1FreshBuild/<session>
```

工具先要求项目的 `DefaultGame.ini` 已将 `Content/Lua` 配置为 UFS staging 目录，再从已提交的 `Lua/` 生成临时 `Content/Lua/*.luac`，依次构建 Editor、Game、Client、Server，并为三种运行目标执行完整 Build/Cook/Stage/Pak/Package。成功后删除临时 Lua 产物及 UAT 生成的 `Build/Windows*` 打包顺序目录，使源码 worktree 恢复干净；`fresh-build-provenance.json` 记录源码提交、引擎与 Lua 编译器指纹、精确命令、日志摘要、Editor 模块 BuildId、Target receipt、PE/Pak 和完整 staged 文件树 SHA-256。任一步骤失败会保留日志及 `fresh-build-failure.json`，不得沿用部分产物。

该报告提供一次本机 clean HEAD 到输出的可审计来源链，但使用共享引擎二进制与缓存，不是 hermetic 或可复现构建证明，也不是受控生产构建服务签章。报告固定保留 `production_build_attestation=false` 和 `reproducible_build_verified=false`，不能单独作为外部验收通过证据。

### 2.2 可选候选冻结本机演练

正式占用验收设备前，可以在完全干净的候选源码 worktree 中演练归档和清单初始化。若日常工作区包含未提交的个人改动，必须另建 detached worktree，不能为了通过检查而删除或暂存这些改动。以下路径都可以使用绝对路径指向已有 staged 构建；`--output` 必须是不存在的新目录：

```powershell
$Commit = git rev-parse HEAD
& $Python Tools/Tests/prepare_phase1_candidate.py `
  --project-root . `
  --source-commit $Commit `
  --creator-module <build-root>/Plugins/AkUGCEditor/Binaries/Win64/UnrealEditor-AkUGCEditor.dll `
  --creator-modules-manifest <build-root>/Plugins/AkUGCEditor/Binaries/Win64/UnrealEditor.modules `
  --game-root <build-root>/Saved/StagedBuilds/Windows/AkUGCPlatform `
  --game-target <build-root>/Binaries/Win64/AkUGCPlatform.target `
  --client-root <build-root>/Saved/StagedBuilds/WindowsClient/AkUGCPlatform `
  --client-target <build-root>/Binaries/Win64/AkUGCPlatformClient.target `
  --server-root <build-root>/Saved/StagedBuilds/WindowsServer/AkUGCPlatform `
  --server-target <build-root>/Binaries/Win64/AkUGCPlatformServer.target `
  --output <build-root>/Saved/Phase1CandidateRehearsal/<session>
```

工具复制实际 Editor 模块，将干净源码中的项目描述和各角色 `.target` 注入对应归档，并归档 Creator 项目文件和三个 staged 可分发树；staged 根目录中运行后生成的 `Saved` 会被排除。工具拒绝私钥、链接、路径碰撞和已有输出，并立即运行与 `init` 相同的 PE、BuildId、Target、Pak 和归档结构检查。产物位于输出目录的 `baseline/`，`candidate-preparation.json` 固定写明 `local_rehearsal=true` 和 `production_acceptance_evidence=false`。随后可将这五个基线文件传给上节 `init` 命令，确认全新 `acceptance.json` 骨架能够生成；不得填写虚构人工证据，也不得执行 `seal` 冒充通过。

本演练只证明声明提交当时干净、归档过程和静态结构检查成功。它不证明输入二进制由该源码提交构建，不证明包能启动或引擎 BuildId 兼容，也不替代实体鼠标、非程序员、真实网络和物理设备验收。正式候选仍须由受控构建流程提供源码到二进制的来源记录。

## 3. 实体鼠标滑动

此步骤独立于 60 分钟可用性计时。测试者和旁观者可以与可用性测试相同，也可以不同。

1. 关闭会抢占前台的程序，并开始屏幕录制或准备滑动前后截图。
2. 从初始化后的清单读取会话 nonce，启动显式人工滑动模式；`--output` 必须是新目录：

```powershell
$Manifest = "Saved/Phase1ExternalAcceptance/<session>/acceptance.json"
$Nonce = (Get-Content $Manifest -Raw | ConvertFrom-Json).session_nonce
& $Python Tools/Tests/verify_copilot_http.py `
  --engine $EngineRoot `
  --mouse-ui --parameter-ui --manual-slider `
  --acceptance-nonce $Nonce `
  --output Saved/Phase1ExternalAcceptance/<session>/manual-slider
```

3. 驱动会用 OS 输入完成新建、字段填写和后续参数回归，因此最终报告应如实保留 `automated_os_input=true`。唯独到达波数滑块检查点后，驱动不移动光标、不聚焦窗口、不发送按下/拖动/释放，并进入等待；测试者必须自行用实体鼠标将光标移到波数 SpinBox 并拖动，把波数从 3 改为 2。不得用键盘改波数，不得运行鼠标宏、远程注入或自动滑块工具。
4. 后续参数检查点完成后，保留：
   - `manual-slider/editor-user/Saved/verification/result.json`
   - 滑动前后截图或录屏
   - 本轮 `os-input-actions.json`
5. 在 `physical_slider` 中填写相对路径，并由旁观者确认 `used_physical_mouse`、`automated_slider_motion_disabled` 和 `observer_attested`。

报告中的 `manual_slider=true`、`numeric_slider_verified=true`、`automated_slider_motion=false` 和 `slider_input_mode=awaited_physical_mouse` 只证明绑定本次运行的 Slate 观察到了目标操作。nonce 会显示在测试窗口标题，并由 Editor 写入全部检查点；Editor 会查询实际已加载的 AkUGCEditor 模块路径、计算其 SHA-256 并与命令行冻结值核对，结束时再次复核。报告仍应保持 `human_ui_validated=false` 与 `physical_input_provenance_verified=false`。真人来源只由外部旁观者声明补足，不能修改报告冒充软件证明。

## 4. 非程序员 60 分钟首作

### 4.1 测试纪律

- 测试者此前不参与本项目开发，并自述不是程序员。
- 开始前取得截图/录屏同意，清单只记录别名，不记录实名或无关个人信息。
- 观察者只读任务目标，不提供点击路径、字段答案或故障修复指导；一旦提供实质指导，本轮作废并重新计时。
- 不运行 OS 输入自动化，不打开 Logic Graph，不复用预制的已完成项目。

### 4.2 给测试者的任务卡

在 60 分钟内完成以下结果：

1. 从 Creator 新建可玩塔防。
2. 使用 L1 参数调整玩法，并做少量场景摆放。
3. 保存项目并确认可玩。
4. 发布并导出 `.akugc` 分享文件。
5. 将分享文件交给另一台物理设备。
6. 通过不同于文件传输的独立渠道发送发布公钥。
7. 接收端核对公钥、验签导入并实际开始游玩。

计时从测试者首次操作产品开始，到接收端成功进入该作品游玩为止。必须大于 0 且不超过 60 分钟。

### 4.3 最少证据

- 开始时间画面。
- Creator 中已完成作品和参数画面。
- 发布/导出成功画面及原始 `.akugc` 文件。
- 接收设备验签导入并进入游玩的画面或录屏。
- 分享文件 SHA-256、Creator 与接收设备的不同设备别名。
- 实际使用的 Creator 完整候选归档 SHA-256，必须等于冻结基线。
- 与发布私钥分离传递的可信发布公钥；清单只保存公钥，不得保存私钥。
- 观察者对“未使用 Logic Graph、未实质指导、独立核钥”的确认。

不得收集 Publisher 私钥、模型 Key 或包含凭据的配置/日志。验证器会拒绝把 `.key` 文件作为证据。

Python 验证器会严格解析 `.akugc` JSON，核对签名清单字段、可信公钥、Ed25519 签名和包内 `releaseId`。它不会复现 UE 对任意 Document/Program 的完整 `ContentHash` 算法；完整内容完整性仍由接收端产品 Loader 的验签导入和实际游玩证据确认。

## 5. 十种子跨设备联机

### 5.0 可选本机四参与者预演

在占用物理设备前，可先用回环网络、NullRHI 和自动化输入预演十种子双轮。Dedicated 模式运行一个打包 Server 和四个打包 Client；Packaged Listen 模式运行一个打包 Game 主机和三个打包 Client。两者都会为每个种子额外验证第 5 个连接被拒绝：

```powershell
$Game = "Saved\StagedBuilds\Windows\AkUGCPlatform\Binaries\Win64\AkUGCPlatform.exe"
$Client = "Saved\StagedBuilds\WindowsClient\AkUGCPlatform\Binaries\Win64\AkUGCPlatformClient.exe"
$Server = "Saved\StagedBuilds\WindowsServer\AkUGCPlatform\Binaries\Win64\AkUGCPlatformServer.exe"

& $Python Tools/Tests/verify_rooms.py --engine $EngineRoot `
  --seeds 10 --rounds 2 --members 4 --client $Client --server $Server `
  --output Saved/LocalFourProcessRehearsal/<session>-dedicated

& $Python Tools/Tests/verify_rooms.py --engine $EngineRoot `
  --listen --packaged-listen --seeds 10 --rounds 2 --members 4 `
  --game $Game --client $Client `
  --output Saved/LocalFourProcessRehearsal/<session>-listen
```

本预演只用于尽早发现包、房间容量、复制和终局一致性问题。它使用同一台计算机、自动化测试作品和回环网络，不验证物理设备、真实局域网、人工操作、视觉、真实文件传递或独立核钥；其日志和结果不得填入 `acceptance.json`，也不得签署为外部门禁证据。

### 5.1 每个种子作品

每台物理设备先解压对应冻结包到本地只读候选目录；不得从共享目录直接运行。开放选定 TCP/UDP 端口，并用实际局域网 IPv4 替换 `$HostIp`。每轮创建新的证据目录，`$Pack` 必须是本轮真实传递到主机设备的 `.akugc`，`$Key` 必须来自独立渠道：

```powershell
$Port = 17777
$HostIp = "192.168.1.20"
$Pack = (Resolve-Path "C:\Phase1\Incoming\<release-id>.akugc").Path
$Key = "<64个小写十六进制字符的Ed25519公钥>"
$Evidence = "C:\Phase1\Evidence\<seed-id>"
New-Item -ItemType Directory -Path $Evidence -ErrorAction Stop | Out-Null
```

Listen 主机设备运行冻结的 Game 包：

```powershell
$GameExe = "C:\Phase1\Game\AkUGCPlatform\Binaries\Win64\AkUGCPlatform.exe"
& $GameExe "/Game/Maps/Phase0?listen?room=1" -AkUGCRoom "-port=$Port" `
  "-LogicPack=$Pack" "-LogicPackKey=$Key" -windowed -ResX=1280 -ResY=800 `
  "-UserDir=$Evidence\host-user" "-abslog=$Evidence\listen-host.log"
```

Dedicated 主机设备改为运行冻结的 Server 包：

```powershell
$ServerExe = "C:\Phase1\Server\AkUGCPlatform\Binaries\Win64\AkUGCPlatformServer.exe"
& $ServerExe /Game/Maps/Phase0 -AkUGCRoom "-port=$Port" `
  "-LogicPack=$Pack" "-LogicPackKey=$Key" `
  "-UserDir=$Evidence\server-user" "-abslog=$Evidence\dedicated-server.log"
```

每台玩家设备运行冻结的 Client 包；Listen 主机本地玩家已由 Game 进程承担，不再重复启动 Client：

```powershell
$ClientExe = "C:\Phase1\Client\AkUGCPlatform\Binaries\Win64\AkUGCPlatformClient.exe"
& $ClientExe "$HostIp`:$Port" -AkUGCRoom -windowed -ResX=1280 -ResY=800 `
  "-UserDir=$Evidence\client-user" "-abslog=$Evidence\client.log"
```

所有玩家进入大厅后分别按 Enter 准备；Listen 主机按 F5 开始，Dedicated 模式由首个加入且成为房主的 Client 按 F5。保留主机“listening on port”、各端加入/开局、实际加载发布 ID、结算和退出日志；端口、主机 IP、进程实例标识和日志路径写入观察记录。每轮结束后完全退出所有进程，再执行下一种子，禁止沿用旧会话。

- 使用不同 `release_id`、签名 `contentHash`、Document/Program 载荷、`.akugc` 文件及 SHA-256；重复改名不算不同作品。
- `release_id` 在清单中使用无连字符的 32 位十六进制；每轮填写独立核对的 64 位小写 `trusted_public_key`。
- 分享文件须真实传递到其他设备，并通过独立渠道核对公钥。
- 每个玩家端填写实际 `package_role`（`client` 或 Listen 主机的 `game`）及对应冻结摘要；Dedicated 主机不计入玩家数。
- 每轮明确填写 `host_mode`：Listen 使用冻结的 Game 包 SHA-256，Dedicated 使用冻结的 Server 包 SHA-256。
- 2–4 台玩家设备必须是不同物理设备；Listen 中恰有一个 `game` 玩家且其设备等于 `host_device`，Dedicated 中所有玩家均为 `client` 且 Server 不计入玩家数；同机多进程不得计入。
- 每个玩家 `participants` 项记录实际加载的 `release_id`、`content_hash`、终局日志和截图。Listen 的 Game 玩家本身就是权威端，`host_observation.participant_index` 引用该玩家，不要求同一进程伪造第二份日志；Dedicated 的 `host_observation` 单独记录 Server 权威端日志。权威端与所有玩家的 `Victory`、团队分、基地生命和资源必须完全相同。
- 每轮填写带时区的 `observed_at_utc` 和观察者别名。
- 10 个样本整体至少包含一轮 2 人和一轮 4 人。
- 至少一个传递后的种子另制作仅改变 Document/Program 载荷、保留原签名信封发布 ID/内容哈希的篡改副本；填写其 SHA-256，并在 `tamper_rejection_evidence` JSON 中绑定该摘要、发布 ID、内容哈希及 `rejected_by_product_loader=true`。原文件继续保留。

初始化清单已经生成 10 轮字段骨架，只需按实际结果填写，不得删除失败轮后用同一证据补位。十轮整体必须同时覆盖 Listen、Dedicated、2人和4人。日志和截图不能跨设备、跨轮或跨证据角色复用。当前终局四项由观察者根据各设备画面/日志人工录入；验证器会核对每端声明的发布 ID、签名内容哈希和候选包角色，比较权威端与玩家端终局值，并由锁绑定原始证据，但不会从游戏日志自动推导终局值，也不构成软件对真实联机结果的独立证明。

`tamper_rejection_evidence` 必须是以下精确结构的 JSON；摘要取自篡改副本：

```json
{
  "tampered_share_sha256": "<64个小写十六进制字符的SHA-256>",
  "release_id": "<原包32位发布ID>",
  "content_hash": "<原包64个小写十六进制字符的签名内容哈希>",
  "rejected_by_product_loader": true
}
```

### 5.2 故障恢复

使用至少两台物理设备分别完成以下四类，每类由观察者填写 `observer_alias` 与带时区的 `observed_at_utc`，并以 `device_reports` 将设备、实际包角色/摘要和日志逐一绑定：

- `client_leave`：在指定 Client 上按 Escape 主动离房，再由同一设备验证干净状态和后续加入。
- `network_loss`：在指定 Client 设备上实际禁用测试网卡或拔线，观察连接失败后恢复网络，并由同一设备完成恢复验证；不得用进程内模拟。
- `host_terminated`：通过任务管理器或 `Stop-Process -Id <Listen主机PID>` 结束 Listen Game 主机；同一主机设备必须启动新 Game 进程完成后续验证。
- `reconnect`：完全退出指定 Client，记录旧进程实例，确认旧会话清理后在同一设备重跑上节 Client 命令，并记录不同的新进程实例。

每类都必须确认：

- `host_mode` 及对应 Game/Server 基线摘要与故障场景一致。
- `host_terminated` 必须是 Listen 主机；该场景与 `reconnect` 必须使用新进程完成后续验证。
- 填写 `fault_device`、`recovery_device`、故障前后进程实例标识，并分别保留故障动作与恢复画面/记录；客户端类故障必须指向 Client，主机终止必须指向 Listen Game 主机。
- 观察者确实看到了故障动作。
- 返回干净的可操作状态。
- 旧 NetDriver、成员/名册和会话分别已清除。
- 随后能够再次加入或完成一次后续游玩。
- 观察者确认实际故障动作和恢复结果。

## 6. 校验与 Go/No-Go

协调者在所有首作、种子和恢复观察时间之后复核原始证据，生成独立签署文件（扫描件或受控电子签署文件），在 `final_attestation` 中填写协调者别名、带时区的 `signed_at_utc`、文件相对路径及 SHA-256，并明确确认人类声明不是软件证明。布尔字段本身不是身份认证；签署文件由最终负责人按组织流程核验。

填写 `acceptance.json` 并确认所有原始证据就位后，先执行一次性封存：

```powershell
& $Python Tools/Tests/phase1_external_acceptance.py seal `
  --input Saved/Phase1ExternalAcceptance/<session>/acceptance.json
```

`seal` 要求初始化时声明的源码提交仍是当前完全干净的 HEAD；若测试期间仓库已切换提交或产生非忽略文件，先恢复原提交和干净状态，不能改写清单。它会对有界 JSON/`.akugc` 使用同一内存快照，对冻结 ZIP 使用同一打开文件绑定结构检查与摘要，对大型证据流式计算摘要，并复查验证期间未变化，再执行第二遍稳定性检查；只有全部通过才会拒绝覆盖地生成 `evidence-lock.json`，其中绑定会话 nonce、原始清单 SHA-256 和每个证据文件 SHA-256。封存期间停止同步工具和其他写入进程。封存后不得修改清单或任何证据。若需修正，必须新建会话目录并重新执行测试，不能删除或覆盖锁文件继续使用原会话。

随后执行最终校验：

```powershell
& $Python Tools/Tests/phase1_external_acceptance.py validate `
  --input Saved/Phase1ExternalAcceptance/<session>/acceptance.json `
  --output Saved/Phase1ExternalAcceptance/<session>/result.json
```

退出码 0 且 `result.json` 中 `passed=true`只表示：

- 所有强制证据存在且非空。
- 冻结基线、清单及全部证据摘要与一次性锁匹配。
- `.akugc` 清单签名、可信公钥、包内发布 ID 及十种子载荷唯一性满足离线检查。
- 首作时间不超过 60 分钟。
- 十个种子、人数覆盖、跨端终局值和四类恢复证据满足协议。
- 人工声明已明确签署，并承认这些声明不是软件独立证明。

任何诊断、缺失证据、摘要变化、跨端数值不一致、复用证据或未签署声明均为 No-Go。`--output` 必须是全新文件，不能指向清单、锁或任一证据；不得手工修改 `result.json` 或 `evidence-lock.json`。未封存的清单不能通过最终 `validate`。

最终 Phase 1 Go 仍需负责人查看原始录屏/截图/日志并签署。正式美术、Android 真机和发布私钥系统级保护按项目既定范围单独跟踪，不因本清单通过而自动完成。

# Phase 2 实施计划

> 基线日期：2026-10-15
> 前置状态：Phase 1 Win64 技术候选已冻结；真人与独立物理设备外部门禁按当前决策延期，不记为通过。
> 阶段目标：在不放松现有签名、确定性与沙箱边界的前提下，引入项目、版本、审核与分发控制面。

## 实施顺序

### P18：发布身份安全化

**目标**：消除 Creator 长期 Ed25519 发布私钥的明文落盘和 UI 暴露，为远端分发建立稳定发布身份。

**实现**：

- Win64 使用当前 Windows 用户范围的 DPAPI 保存版本化 `publisher-identity.json`。
- 产品发布路径只允许读取公钥或请求签名，不向 UI、命令行结果、日志或分享包返回私钥。
- 首次使用时验证并迁移旧 `private.key`；受保护身份与旧密钥冲突、密文损坏或当前用户无法解密时明确拒绝，绝不静默换钥。
- 身份变更使用跨进程独占文件锁、新建与原子移动；迁移后将旧明文内容替换为不含秘密的兼容阻断标记，使旧版程序拒绝发布而不能静默创建第二身份。该替换不等于 SSD/备份介质上的取证级擦除，不作该声明。
- 崩溃遗留的 `private.key.<id>.tmp` 或 `publisher-identity.json.<id>.tmp` 会阻止继续发布，并同正式身份文件一样被候选归档和证据工具拒绝。
- Creator、PackTool 和本地作品库统一走受保护发布入口；底层显式密钥签名 API 仅保留给内存夹具和已有密钥工具。
- 候选归档与外部证据验证器将 `publisher-identity.json` 视为敏感私钥凭据，即使其内容已由 DPAPI 加密也禁止收集。

**已验证（2026-10-15）**：

- AkUGCPlatformEditor、Client、Server Win64 Development 编译成功。
- Python 回归 103/103；Editor AkUGC 自动化 125/125。
- 定向覆盖身份创建与重启复用、旧明文迁移、并发创建收敛、敏感临时文件阻断、DPAPI 密文篡改拒绝、拒绝后不覆盖、发布/导入/验签隔离。
- `verify_work_library.py` 完成 PackTool 安全发布、未信任与篡改拒绝、独立接收库导入、重启复玩及真实渲染结算；发布目录同时生成受保护身份、无秘密兼容阻断标记和跨进程锁文件。证据位于本机 `Saved/WorkLibraryVerification/04624657a1e14e328807bc1c8483d641/result.json`。

**边界**：

- 当前仅 Win64 Creator 支持创建和使用发布身份；Android 与其他平台不会回退明文。
- DPAPI 当前用户密文不可作为跨机器备份，也不能通过复制 `publisher-identity.json` 恢复到另一用户。

### P18.1：发布身份恢复与轮换

**目标**：在不导出明文私钥、不允许静默换钥或历史回退的前提下，支持身份丢失恢复，并为 P19 建立可验证的作者公钥演进链。

**实现**：

- Creator 可导出 `.akugcrecovery` 恢复包；使用 scrypt `N=131072, r=8, p=1`、16 字节随机盐派生 256 位密钥，以 12 字节随机 nonce 的 AES-256-GCM 加密 Ed25519 私钥，并把当前公钥、密钥版本和轮换历史作为认证附加数据。
- 恢复包只允许导入空身份目录、与现有私钥及完整历史完全相同时幂等成功，或在现有 DPAPI 密文无法由当前用户解密但公开身份元数据完全匹配时重新封装；错误密码、篡改、另一身份和旧历史回退均拒绝且不修改当前身份。
- 显式轮换同时生成新 Ed25519 身份、由旧私钥和新私钥共同签署轮换声明、追加连续版本历史，并在切换前为新身份生成加密恢复包。
- 轮换签名消息固定为 UTF-8 文本 `AkUGCPlatform.PublisherRotation.v1`，后续按行写入十进制 `sequence`、`createdAtUnixSeconds`、小写 hex `previousPublicKey` 和 `newPublicKey`；P19 必须按同一字节协议验证旧、新两个 Ed25519 签名。
- `publisher-identity.json` 升级为版本 2，单文件原子保存当前 DPAPI 密文与公开轮换历史；版本 1 身份保持兼容，只在轮换或恢复写入时升级。
- 作品库面板提供备份、恢复和需显式确认的“轮换并备份”入口；密码只保留在操作期间并在完成后清空。
- 恢复包拒绝覆盖已有文件；崩溃遗留的 `.akugcrecovery.<id>.tmp` 会阻止同路径重试。候选归档和外部证据工具把正式恢复包及其临时文件与私钥文件同等处理，即使已加密也禁止收集。

**已验证（2026-10-15）**：

- AkUGCPlatformEditor、Client、Server Win64 Development 编译成功；Python 回归 106/106，Editor AkUGC 自动化 125/125。
- 自动化覆盖错误密码、密文篡改、恢复到空目录、重复恢复幂等、不可解密 DPAPI 身份的同身份重封装、现有身份冲突、旧备份回退拒绝、恢复后确定性签名一致、轮换双签名、完整历史恢复、恢复包覆盖及临时文件阻断。
- 版本 2 身份继续完成安全发布、未信任与篡改拒绝、隔离接收库导入、重启复玩和真实渲染结算；证据位于本机 `Saved/WorkLibraryVerification/629c43c0780e41eaaa22c6666e78ff50/result.json`。

**安全边界**：

- 恢复包与密码任一丢失都无法恢复；二者放在同一位置等同于失去保护。
- 轮换不改变旧作品签名。P19 必须验证并登记完整双签名链，旧公钥是否仍可提交新 Release 由 Registry 状态决定。
- 完全丢失私钥且没有恢复包时，密码学上不能证明新身份继承旧身份；只能进入后续账号人工恢复流程，不能本地伪造连续性。

### P18.2：不可信输入预算与解析前拒绝

**目标**：在进入 JSON DOM、UStruct、Lua 编译器或文件内容分配前统一限制不可信输入，防止超大文件、多字节绕过、深层/高 token JSON、重复字段、数组放大、非法索引和路径穿越。

**实现**：

- 新增共享 `AkUGCInputBudget`：Project/Command JSON 上限 4 MiB、Logic Pack 16 MiB、Copilot 请求/提案 64 KiB、响应 256 KiB、解释 16 KiB；所有文本按 UTF-8 字节计量。
- JSON 进入 DOM 前统一限制嵌套深度 32、token 131072、字段名 128 字符，并拒绝普通或转义形式的重复字段。
- Document 直接反序列化也执行场景、实体、组件、属性、Tag、Logic、Ruleset 与字符串结构预算；Command 事务最多 1024 条；Logic Pack 对 Manifest、依赖、指令、边、入口/后继索引和签名字段先行限界。
- 项目、Logic Pack、作品库和发布身份文件统一通过同一文件句柄先取长度、再有界读取，避免先检查路径再无界重开的竞态；拒绝不会替换现有项目、身份或作品记录。
- Copilot 在面板序列化后和 Transport 内重复检查出站 JSON；URL、API Key、响应体、内层提案与解释分别限界，超限、深层或重复字段响应不产生可应用预览。
- Lua 模块名仅允许规范 ASCII 分段并限制在配置根目录内，单模块最多 4 MiB；沙箱 Lua 源码最多 128 KiB、chunk 名最多 256 UTF-8 字节，超限在 Lua 编译前以独立状态拒绝。
- Ed25519 私钥、公钥和签名 Hex 在解码分配前要求精确长度，避免低层 API 被超长字符串放大。

**已验证（2026-10-15）**：

- AkUGCPlatformEditor、Client、Server Win64 Development 编译成功。
- Python 回归 106/106；Editor `AkUGC` 自动化 128/128。
- 自动化覆盖多字节字节预算、深度/token 放大、普通与转义重复字段、Document 结构与字符串预算、Command 数量、Pack 非法后继索引、项目/App 真实加载入口、Copilot 出站/提案边界、身份重复字段、超长签名 Hex、Lua 模块路径/文件上限与沙箱源码上限。
- 完整 Editor 日志位于本机 `Saved/Logs/InputBudget_FinalAutomation.log`。

**安全边界**：

- 输入预算只限制解析与资源放大，不替代 Schema、Capability、内容哈希、签名、业务规则和移动端内容预算；客户端与服务端仍须执行完整本地验证。
- 当前上限面向首个 Creator Alpha；如需调整必须同时更新共享常量、入口测试和运行预算，不能在单一路径放宽形成旁路。
- 四台真实物理设备的外部验证继续按当前决策延期，不计入本阶段通过证据。

### P18.3：沙箱副作用与挂起资源配额

**目标**：确保内容脚本对宿主的消息、伤害、生成、查询和定时任务都在产生副作用或保留资源前完成不可绕过的计数，防止单次执行、跨 `RunScript` 累积和单 Tick 回调洪泛。

**实现**：

- 默认启用总副作用 64、消息 32、伤害 32、生成 16、单次执行定时器注册 16、宿主查询 128、跨运行挂起定时器 64 和单次 `AdvanceTimers` 回调 16 的分层配额；上述配置必须全部为正数，不能再以 `0` 关闭。
- 消息、伤害、生成和定时器注册在调用宿主或创建 Lua registry 引用前同时消费总量及分类型配额；查询独立计数。任一配额拒绝会闭锁当前执行，Lua `pcall` 即使捕获局部错误，最终结果仍为 `EffectLimitExceeded`，后续宿主调用也不会继续执行。
- 消息限制为 1024 UTF-8 字节，实体/Prefab/Anchor 标识符限制为 128 UTF-8 字节；逐字节拒绝截断、过长编码、代理项、越界码点和嵌入 NUL。伤害必须位于 `(0, 1000000]`，定时器延迟必须位于 `(0, 3600]`，并以位级检查拒绝 NaN 和正负无穷。
- 原始 native 受控 API 只返回结构化的 `ok, values...`，由初始化时创建的可信 Lua 包装器在 C 回调正常返回后转换为脚本错误；`AkUGCSandbox` 同时启用 C++ 异常展开供 Lua hook 使用。恶意类型、非法 UTF-8 或非有限数值不再能让异常跨 native 回调边界中止宿主进程。
- 定时器先统一推进本 Tick 已存在的任务，再按批次上限触发；超出批次的到期任务保留到下一次推进。回调引用和 pending 项在调用前释放，回调内新注册定时器从下一 Tick 开始计时，避免数组失效、同 Tick 递归触发和 registry 泄漏。
- 修复 P18.2 输入预算在 Unity 合并编译时与其他 `.cpp` 匿名命名空间 `Fail` 符号发生歧义的问题，跨文件调用改为完整命名空间限定。

**已验证（2026-10-15）**：

- AkUGCPlatformEditor、Client、Server Win64 Development 编译成功。
- Python 回归 106/106；Editor `AkUGC` 自动化 134/134，其中沙箱专项 22/22。
- 自动化覆盖安全默认值、总量与分类型配额、宿主调用前拒绝、`pcall` 闭锁、查询上限、超长/NUL/非法 UTF-8、错误参数类型、非有限伤害与延迟、单次注册上限、跨运行 pending 累计、单次推进回调上限及回调内安全注册。
- 完整 Editor 日志位于本机 `Saved/Logs/SandboxEffectQuota_FinalAutomation.log`。

**安全边界**：

- 配额配置属于可信产品代码，不能由作品内容修改；扩大上限必须同时评估“单回调配额 × 单次推进回调数”的最坏 Tick 成本。
- 定时器仍是进程内短期任务，不提供持久化或跨会话恢复；宿主实现仍须独立验证实体、Prefab、规则状态和权限。
- 四台真实物理设备的外部验证继续按当前决策延期，不计入本阶段通过证据。

### P18.4：自动化最终判定不可误报

**目标**：消除 Unreal Automation 失败但进程仍返回 `0`、测试未发现、日志被截断或驱动只看业务标记时产生的假通过，确保后续 P19 门禁使用同一套可审计的最终判定。

**实现**：

- 新增共享 `unreal_automation_verdict.py`，同时解析测试发现、逐项启动、逐项最终结果、队列结束数量和 `TestExit`；发现数、启动集合、最终集合与队列数量必须一致，任一非 `Success`、重复最终结果、零测试或缺少结构性收尾均拒绝。
- 精确测试驱动必须证明预期完整 Path 恰好启动并成功一次；完整 `AkUGC` 门禁额外设置 134 项下限，避免命令选择器意外缩小后仅凭少量成功结果通过。
- 所有运行都要求唯一队列结束标记；退出型运行还要求进程码为 `0` 及唯一 `TestExit`，持续运行的联机进程则必须在收集最终证据时仍存活。超时始终是独立失败条件，不能由已有成功片段覆盖。
- Copilot HTTP/UI、Work Library、Network Visual、Solo Playable、房间恢复、OS 键盘双局和多客户端房间驱动全部改用共享判定；Automation 最终判定只读取包含队列末尾且不会与另一通道重复计数的独立 stdout，`-abslog` 继续用于业务事件证据，不再把两者拼接后当作独立结果。
- 保留业务级 Passed 标记、Lua 启动、截图、结算和隔离目录检查；共享判定只替代不完整的 Automation 收尾判断，不弱化现有产品验收条件。

**已验证（2026-10-15）**：

- Python 回归 120/120。
- 判定器回归覆盖完整成功、持续进程成功、失败但退出码为 `0`、零测试、头尾日志截断、重复最终结果、启动后未完成、事件顺序错误、超时、非零进程码、队列数量不一致、预期 Path 缺失和完整套件数量回退。
- 使用真实 134 项 Editor 日志复核发现、启动、完成和队列数量均为 134，且 `TestExit` 唯一；最终回归日志位于本机 `Saved/Logs/TestVerdict_FinalAutomation.log`。
- `verify_copilot_http.py --all-tests` 通过共享判定器完成 134/134，并同时完成 42 个本地 HTTP/TLS 协议夹具；证据位于本机 `Saved/TestVerdictCopilot_Final/result.json`。

**边界**：

- 判定依赖 Unreal 当前稳定的 Automation 结构化日志格式；引擎升级若改变标记必须先更新解析器及夹具，解析失败按拒绝处理。
- 持续运行的联机场景没有 `TestExit` 进程退出标记，但仍要求队列为空；共享判定确认精确测试结果后，再由各驱动确认进程存活及业务证据。这不等同于四台真实物理设备验收。
- 四台真实物理设备的外部验证继续按当前决策延期，不计入本阶段通过证据。

### P19：Release Registry 最小闭环

**目标**：建立可本地部署的 Go + PostgreSQL 发布控制面，先完成不可变 Release 的上传、审核、发布、撤回和下载，不在本阶段开放玩家资产上传。

首个验收切片：

1. Creator 上传已签名 `.akugc`，服务端绑定 `ReleaseId`、内容哈希、包摘要与发布公钥。
2. 状态仅允许按 `submitted → approved/rejected → published/revoked` 规则迁移，已登记发布内容不可覆盖。
3. 仅 `published` Release 可下载；客户端仍必须使用现有本地验签与内容哈希校验，不能信任服务端绕过。
4. 第二个隔离用户下载、导入并完成一局；篡改包、Release ID 冲突、撤回版本和重试竞争均有自动化拒绝证据。

### P19.1：不可变 Registry 与本地分发闭环

**实现**：

- 新增 `Services/ReleaseRegistry` Go 服务与版本化契约。Bearer 主体分为 Creator、Reviewer、Publisher 和 Player；本地认证配置只保存 token SHA-256 摘要，Creator 主体显式绑定允许使用的 Ed25519 发布公钥。
- 上传入口在 16 MiB 预算内拒绝非法 UTF-8、过深/过多 token、重复字段、未知包字段、非法 UUID/hex 与无效 Manifest Ed25519 签名；Go 确定性 Manifest 编码已与 Unreal 实际签名夹具互验。
- Go 的快速外层验签之后必须调用 `AkUGCPackTool -ValidateWork`。该受信任命令复用 `FAkUGCLogicPackLoader::LoadVerified`，重新检查签发公钥、ContentHash、Document、Logic IR 与能力白名单；验证器缺失、超时或拒绝时上传失败，不允许降级。
- PostgreSQL 保存不超过 16 MiB 的首版制品及 SHA-256，使用主键、Check、不可变字段触发器、`Owner + Idempotency-Key` 事务级 advisory lock 和 Release 行级锁保护重复提交与状态竞争；所有实际状态变化在同一事务追加审计记录。
- 状态固定为 `submitted → approved/rejected`、`approved → published`、`published → revoked`。Reviewer 不能审核自己的作品；只有 Owner + Publisher 可发布或撤回；`rejected` 与 `revoked` 不可下载，重复目标操作幂等，其他竞争返回明确冲突。
- `Tools/Tests/verify_release_registry.py` 使用临时 PostgreSQL、独立 Creator/Reviewer/Player token 和独立作品库运行真实 HTTP 流程，并在下载后再次执行本地深度验证和 Packaged Client 游戏回归。四台物理设备仍由本机隔离角色代替。
- 详细协议与当前边界记录在 `Docs/Phase2/ReleaseRegistryContract.md`；公钥轮换链登记、对象存储、私人可见性、协作者和构建任务留在后续 P19/P20 切片。

**已验证（2026-10-15）**：

- AkUGCPlatformEditor、Client、Server Win64 Development 编译成功；Python 回归 120/120。
- Go 单元、HTTP、内存并发与真实 PostgreSQL 隔离 Schema 集成测试通过，`go vet ./...` 通过；Windows 当前未启用 CGO，因此本轮未执行 `go test -race`。
- Editor `AkUGC` 完整自动化 134/134，Copilot 本地 HTTP/TLS 协议夹具 42/42。
- 最终本机隔离闭环的 8 路并发提交产生一个 `201` 和七个 `200`，并完成未授权上传、篡改、Release ID 冲突、未发布下载、撤回下载拒绝，以及 Player 下载后本地深度验签和一局结算；证据位于本机 `Saved/ReleaseRegistryVerification/P19_Final3/result.json`。

**边界**：

- 当前静态 Bearer 认证和无 TLS HTTP 被强制限制在 loopback，仅用于本机 Alpha；公网部署必须替换正式身份提供方并启用 TLS。
- 当前 Creator/Reviewer/Player 隔离是同一台机器上的独立 token、目录和进程，不等同于四台物理设备验收。
- P19.1 提交时 Creator Studio 与 Player 尚未直接调用 Registry；本切片由真实 PackTool、HTTP、PostgreSQL 和 Packaged Client 编排证明服务契约，产品入口由后续切片分别接入。

### P19.2：Creator Studio Registry 产品接入

**目标**：让 Creator 在现有本地签名发布之后，直接从作品库面板提交不可变 Release、查看审核状态并发布已审核版本；不把 Reviewer 权限或公网凭据模型混入 Creator。

**实现**：

- 将 Editor 的 libcurl、TLS、禁重定向、接收时响应预算、超时和取消逻辑抽为共享异步传输，Copilot 与 Registry 共用最多四个工作请求和统一生命周期，模块卸载前会取消并等待所有请求。
- 新增 Creator Registry 客户端。P19 静态 token 模式只接受 `http://127.0.0.1` 或 `http://localhost`，拒绝用户信息、路径、查询、片段、非法端口、非 ASCII token 和 Header 注入；token 仅保留在当前面板内存，不写入项目或配置。
- 上传前通过单一文件句柄限制制品为 16 MiB，并再次用最近一次本地发布记录的独立公钥、ReleaseId 和 ContentHash 调用 `LoadVerified`；磁盘文件被替换或与最近签名结果不一致时不发送。
- 提交使用稳定的 ReleaseId 幂等键和原始 `.akugc` 字节；控制面响应在接收时限制为 64 KiB，要求规范 UTF-8、无重复字段、无未知字段、精确类型和受支持状态，并绑定回本地 ReleaseId、公钥与 ContentHash。服务端返回的公钥不能成为新的信任根。
- 作品库面板新增“上传最近签名包”“刷新 Registry 状态”“发布已审核 Release”和取消请求。上传、查询、失败与发布均不修改当前 Document、本地作品库或签名制品；Reviewer 仍必须使用另一主体在 Creator UI 外完成审核。
- 本地协议夹具验证真实二进制上传、Bearer、Content-Type、幂等键、GET 和空体发布请求；严格解析回归覆盖 loopback 边界、重复/未知字段、错误公钥、非法 UTF-8、超限、取消和非预期 HTTP 状态。

**已验证（2026-10-15）**：

- AkUGCPlatformEditor Win64 Development 编译成功；Python 回归 120/120。
- Editor `AkUGC` 完整自动化 137/137，原 Copilot HTTP/TLS 42/42 保持通过，Creator Registry 协议完成 submit/get/publish 三步且未修改项目修订或文档。
- 最终证据位于本机 `Saved/P19_2_CreatorRegistry_Final4/result.json`。

**边界**：

- 当前面板不提供 Reviewer、撤回、公钥轮换链登记或 token 持久化；公网 IdP、TLS、云项目和协作者仍不属于本切片。
- P19.2 本身只接入 Creator；Player 产品内直接获取、导入和游玩已在后续 P19.3 完成。
- 四台真实物理设备验收继续延期，不计入本阶段通过证据。

### P19.3：Packaged Player Registry 产品接入

**目标**：让 Packaged Player 使用 Player 身份直接获取 published Release，并确保 Registry、HTTP Header 或包内自带公钥都不能替代独立信任根和本地完整验证。

**实现**：

- 将 P19.2 的异步 libcurl 传输下沉为 `AkUGCCore` Runtime 能力，Editor 保持兼容入口；Client 启动时统一初始化，退出前取消并回收工作线程。传输继续禁用代理、netrc 和重定向，并新增 64 KiB 响应 Header 总预算、`Content-Length` 接收前超限拒绝、最终 URL 与响应 Header 证据。
- 新增 Player Registry 客户端和 `FetchRegistryWork` / `CancelRegistryWork` 入口。静态 token 阶段只允许规范 loopback Base URL，token 只保留到请求完成，不写入配置、本地作品记录或日志。
- 下载严格限制为 16 MiB、未压缩且有固定长度的 `application/vnd.akugc+json`；要求唯一 `Content-Length`、Artifact SHA-256 ETag 和 ContentHash Header，并核对精确响应字节、最终 URL、重复字段及禁止的传输编码。
- 信任公钥必须由 Player 调用方独立提供。客户端不请求或采用 Registry 的 `publisherPublicKey`；HTTP 200、ETag、ContentHash 和包内公钥都不能把错误信任根提升为可信。
- 作品库新增原始制品字节导入边界：规范 UTF-8 后调用 `LoadVerified`，并在任何写盘前绑定请求 ReleaseId 与已签名 ContentHash。入库后的 `PrepareLibraryWork` 继续从 `.ugcwork` 再次验签，成功准备后才允许 `StartSolo`。
- Packaged Client 验收先后验证未发布 404、错误 token 401、错误独立公钥、具有匹配 ETag 的网络篡改包、Header 阶段 16 MiB + 1、HTTP 302、正常直接下载并完成一局，以及撤回后的 404；每个拒绝都确认作品库为空且 Session 未启动。

**已验证（2026-10-15）**：

- AkUGCPlatformEditor、Client、Server Win64 Development 编译成功；最新 Client 完成 Cook/Stage。
- Python 回归 120/120；Editor `AkUGC` 完整自动化 139/139；共享 Copilot HTTP/TLS 协议夹具 42/42 保持通过。
- 真实 PostgreSQL + Registry + Packaged Player 本机隔离闭环通过：8 路提交为一个 `201` 与七个 `200`，Player 产品入口完成全部拒绝门禁、本地验签、入库、准备、开局、胜利结算和撤回后拒绝。
- 完整证据位于本机 `Saved/ReleaseRegistryVerification/P19_3_PlayerRegistry_Final3/result.json`；共享传输与完整 Editor 回归证据位于 `Saved/P19_3_Copilot_Final1/result.json`。

**边界**：

- 当前 Player 是本机静态 token + loopback HTTP Alpha 入口；正式账号登录、凭据安全存储、TLS、公网错误恢复和离线下载体验仍需后续产品化。
- 本切片只接入单人 Player。Dedicated Server 固定 Release 获取、会话版本锁定和回滚仍未实现，不能把 Player 本地库当作 DS 发布控制面。
- 四台真实物理设备验收继续延期；本轮证据是同机独立 token、目录、进程和恶意 loopback 夹具，不计作实体设备通过。

### P20.1：云项目、不可变版本与协作者权限基础

**目标**：在已稳定的 Release Registry 上建立 Project 控制面，让一个签名 Project 拥有唯一 Owner、受控 Editor/Viewer 协作者和有界不可变版本历史；先证明服务端权限、不变量和并发语义，再接 Creator Studio 项目界面。

**实现**：

- 新增 Project、ProjectMember、项目创建幂等记录和成员审计事件。ProjectId 与 Portable Project 及签名 Manifest 使用同一非零规范 UUID；DisplayName、Owner 和创建时间在本切片不可修改，成员变化才增加 Project Revision。
- Owner 可幂等添加、修改和移除 Editor/Viewer，但不能删除或降级自身。项目外主体读取项目统一返回 404；成员列表不暴露 token 或发布密钥配置。
- Release 提交必须先证明 Project 已登记，且调用者同时具备全局 Creator、登记签名公钥和项目 Owner/Editor 权限。服务在深度验包前快速拒绝，并在 PostgreSQL 提交事务内再次锁定 Project 检查，避免成员移除与上传之间的检查时使用竞态。
- Release 的 `ownerSubject` 固定为 Project Owner，新增不可变 `submittedBySubject` 记录实际首次提交者。Editor 不能因提交版本获得发布或撤回权；Owner、Editor、Viewer 即使同时具有 Reviewer 角色也不能审核本项目。
- 新增严格的项目创建、读取、成员增删和版本列表 HTTP API。版本按 SubmittedAt 与 ReleaseId 稳定倒序，`limit` 限制为 1–100，Cursor 必须属于同一项目；响应只含 Release 元数据，不加载或返回制品字节。
- PostgreSQL migration 会为 Owner 唯一的 P19 历史 Release 回填 Project，并增加 Project 外键、提交者字段、项目/Release 不可变触发器及 Owner 成员冲突触发器；同一历史 Project 已出现多个 Owner 时失败关闭。
- `verify_release_registry.py` 在真实 PostgreSQL 流程中增加项目幂等登记、Editor/Viewer 授权、项目外隐藏、Editor 并发提交归属、版本查询和移除后上传拒绝，同时保留 P19 的篡改、ID 冲突、审核、发布、下载、本地验签、撤回与 Packaged Player 门禁。

**已验证（2026-10-15）**：

- Go 单元、HTTP、内存并发和真实 PostgreSQL 隔离 Schema 测试通过，`go vet ./...` 通过；Project 创建并发只产生一次创建，成员变化、Release 状态和不可变触发器保持事务一致。
- Python 回归 120/120；AkUGCPlatformEditor Win64 Development 编译成功；Editor `AkUGC` 完整自动化 139/139，Copilot/Creator Registry HTTP 与 TLS 协议夹具 42/42 保持通过。
- 真实 PostgreSQL + Registry + Packaged Player 最终闭环通过：8 路 Editor 重试得到一个 `201` 和七个 `200`，Release Owner 固定为 Project Owner、实际提交者为 Editor；移除 Editor 后旧 token 上传返回 `403`，Viewer 有界版本查询和项目外 `404` 通过。
- P19 Player 门禁未回退：未发布、错误 token、错误独立公钥、传输篡改、Header 超限、重定向和撤回后下载均拒绝；正常版本完成本地验签、入库、准备、开局和胜利结算。
- 完整证据位于本机 `Saved/ReleaseRegistryVerification/P20_1_Final1/result.json`；Editor 与共享协议回归证据位于 `Saved/P20_1_Copilot_Final1/result.json`。

**边界**：

- P20.1 只完成 Registry 基础与本机协议编排；Creator Studio 尚无项目创建、成员管理和版本历史界面，现有 Creator 上传前需由编排或后续产品入口登记同一 ProjectId。
- 静态 Bearer、loopback HTTP 与本机角色隔离仍不是公网身份系统；邀请接受、组织、所有权转移、密钥轮换登记和 TLS/IdP 留给后续切片。
- 私人/公开作品页、构建任务与对象存储、DS 固定版本/回滚、运营观测和 Asset Pack 均未进入本切片。
- 四台真实物理设备验收继续按当前决策延期，不计入 P20.1 通过证据。

玩家 Asset Pack 白名单试点放在 Release Registry 稳定之后，避免同时扩大网络、内容与沙箱攻击面。

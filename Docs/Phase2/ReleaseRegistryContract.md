# Release Registry v1 契约

> 状态：P19 最小分发闭环与 P20.4 异步可信构建/制品分发；2026-10-15。
> 范围：云项目、协作者、发布可见性、作品页，以及不可变 `.akugc` 版本的提交、异步深度验证、审核、发布、撤回和下载。

## 信任边界

1. Registry 不签署作品，也不替 Creator 生成发布身份。
2. 上传包上限为 16 MiB。Go 入口先拒绝非法 UTF-8、深度或 token 超限 JSON、重复字段、未知包字段、非法 GUID/hex 和无效 Ed25519 Manifest 签名。
3. Go 入口必须把包内公钥绑定到已认证 Creator 的登记公钥；不能接受“包自己声明的任意可信公钥”。
4. 新提交在请求内完成严格外层解析、签名与 ACL 检查后进入 `queued` 构建；后台 Worker 必须调用受信任的 `AkUGCCore` 深度验证器，重新检查 ContentHash、Document、Logic IR、能力白名单和签名。验证器不可用时构建失败且可显式重试，不能降级为成功或允许审核、发布。
5. Registry 返回 `published` 包不代表客户端可以跳过验证。Creator、Player 和 DS 下载后仍以本地受信公钥调用现有 `FAkUGCLogicPackLoader::LoadVerified`。
6. 本地模拟使用静态 Bearer Token 摘要配置；配置只保存 SHA-256 token 摘要，服务强制只监听 loopback。该认证器不是公网账号系统，公网部署前必须替换为正式身份提供方和 TLS 终结。
7. 作品页中的 ReleaseId、ContentHash、摘要和 `publisherPublicKey` 都只是 Registry 元数据；Player 只能使用调用方独立提供的 Ed25519 公钥建立信任。
8. `.akugc` 已是 Creator 构建并签名的最终运行制品。P20.4 的“构建”只异步深度验证并提升完全相同的字节；Registry 不执行逐作品 Unreal Cook，不改写 Manifest/Document/Logic，不重新签名或代签。

## Release 不变量

- `releaseId` 是全局主键；规范形式为小写带连字符 UUID。
- `projectId`、Schema、Owner、实际提交者、PublisherPublicKey、ContentHash、ArtifactSHA256、ArtifactSize、Artifact bytes 和 SubmittedAt 创建后不可修改；从历史 PostgreSQL `bytea` 清空到已经验证的内容寻址对象只改变物理存放位置，不改变逻辑字节或元数据。
- `ownerSubject` 永远取项目 Owner，`submittedBySubject` 记录首次创建该版本的实际 Owner 或 Editor。相同制品由另一名授权协作者重试不会改写首次提交者。
- PostgreSQL 约束与触发器共同保护不可变字段；服务层不能用状态更新覆盖内容字段。
- 同一 Owner、同一 ReleaseId、同一完整制品重复提交返回现有记录，不重置状态。
- 同一 ReleaseId 的任一不可变字段或制品摘要不同，返回 `409 conflict`，不修改已有记录。Manifest 签名仍有效但 Document 或 Logic 字节已被改动时同样适用：提交入口不能把冲突字节送入构建或覆盖已登记制品。ContentHash、Document 和 Logic IR 的深度复核仍由异步 Worker 对真正的新提交执行。
- `Idempotency-Key` 在同一 Owner 内绑定首次请求的 ArtifactSHA256；相同键和相同制品幂等，相同键和不同制品返回 `409 conflict`。

## 状态机与权限

- `submitted → approved`：只能由具备 Reviewer 角色且不是作品 Owner 的主体执行。
- `submitted → rejected`：只能由独立 Reviewer 执行，且必须提供拒绝原因；`rejected` 为终态，修改后需使用新 ReleaseId 重新提交。
- `approved → published`：只能由作品 Owner 且具备 Publisher 角色的主体执行。
- `published → revoked`：只能由作品 Owner 且具备 Publisher 角色的主体执行，且必须提供撤回原因。
- 对已处于目标状态的相同操作返回幂等成功；其他跨状态、逆向或竞争失败返回 `409 conflict`。
- 每次实际状态变化都在同一数据库事务内追加审计记录。
- `approved|rejected` 审核和 `published` 发布都额外要求同一 Release 的可信构建已经 `succeeded`；构建排队、运行或失败时返回冲突，不能由 Reviewer/Publisher 绕过。

## HTTP v1

- `POST /v1/releases`
  - Bearer：Creator。
  - `Content-Type: application/vnd.akugc+json`。
  - 必须提供 16–128 位安全 ASCII `Idempotency-Key`。
  - 新建返回 `201`，幂等重试返回 `200`。
  - 响应 JSON 精确包含 `created` 与 `release`。构建任务不嵌入提交响应，只能通过独立的 `GET /build` 读取。
- `GET /v1/releases/{releaseId}`
  - Owner、Reviewer，或查看已发布版本的 Player 可访问元数据。
- `POST /v1/releases/{releaseId}/review`
  - Bearer：Reviewer。
  - JSON：`{"decision":"approved|rejected","reason":"..."}`。
- `POST /v1/releases/{releaseId}/publish`
  - Bearer：Owner + Publisher；请求体必须为空。
- `GET /v1/releases/{releaseId}/build`
  - Bearer：独立 Reviewer，或该 Project 的 Owner/Editor/Viewer；项目外 Player 即使作品公开也不能读取内部构建状态。
- `POST /v1/releases/{releaseId}/build/retry`
  - Bearer：同时具备 Creator 平台角色的 Project Owner/Editor；请求体必须为空。
  - 只有 `failed + retryable` 可恢复到 `queued`；重试不创建新 Release、不改变签名制品。
- `POST /v1/releases/{releaseId}/revoke`
  - Bearer：Owner + Publisher。
  - JSON：`{"reason":"..."}`。
- `GET /v1/releases/{releaseId}/download`
  - Bearer：Player。
  - 只有 `published` 且 Project 为 `public`，或 Player 同时是该私人 Project 的成员时返回制品；其他情况统一按 `404` 隐藏。
  - 响应携带 Artifact SHA-256 ETag 和 ContentHash，但二者都不能替代 Player 本地验签。

## Creator Studio 客户端

- 面板只操作最近一次成功的本地签名发布；上传前重新读取并以该记录的独立公钥执行 `LoadVerified`，同时绑定 ReleaseId、ContentHash、发布公钥、Pack Manifest ProjectId、Document Manifest ProjectId、当前 Document ProjectId 和作品名称，不接受任意手工路径或云端元数据绕过。
- 静态 token 阶段仅允许 loopback HTTP；Base URL、Creator token 和请求状态只存在于面板内存，不写入项目、配置或作品库。
- 上传前先读取当前 Project。存在时只有 Owner/Editor 可继续；404 时使用 `akugc-project-<ProjectIdDigits>` 和当前 Document DisplayName 创建，创建并发返回 409 时重新读取权限后再决定是否提交。Viewer、项目外主体和 Project 绑定不一致都明确停止，不发送制品。
- 上传使用 `akugc-release-<ReleaseIdDigits>` 作为稳定幂等键。重复点击或重启后的相同包由 Registry 返回现有 Release，不创建新版本。
- submit/get/publish/build/retry 以及 Project/Member/Version 响应最多 64 KiB，必须是规范 UTF-8、无重复和未知字段的契约 JSON；返回的 ProjectId、ReleaseId、公钥、ContentHash、输入/输出 ArtifactSHA256 与 ArtifactSize 必须与当前 Document 和本地签名字节一致。
- P20.1 响应新增必需的 `submittedBySubject`；Creator 严格解析器同步校验该字段，避免真实 Registry 与本地协议夹具漂移。
- P20.2 面板按 Project → Members → Versions 串行刷新，版本请求固定 `limit=100`；若返回 `nextCursor`，界面必须说明只展示最新 100 条，不能暗示历史完整。
- P20.3 Project 响应新增必需的 `visibility`；只有 Owner 可从面板切换 `private|public`。相同目标幂等成功且不增加 Revision，实际变化完成后刷新 Project、Members 和 Versions。
- 面板必须明确说明：公开只影响项目外 Player 的作品页与下载权限，只暴露仍为 `published` 的版本；切换可见性不修改、不重新签名本地制品，也不把 Registry 公钥提升为信任根。
- P20.4 提交或刷新 Release 后继续读取独立构建状态。只有输出摘要和大小与本地精确字节一致的 `succeeded` 才允许在界面发起 publish；`failed + retryable` 可显式重新排队，终态验证拒绝必须修复内容并生成新 ReleaseId。
- 只有当前 Project 的 Owner 可以从面板设置或移除成员；Subject 必须匹配 `[a-zA-Z0-9][a-zA-Z0-9._:-]{0,127}`，可设置角色只有 Editor/Viewer，不能通过客户端请求修改 Owner。
- Registry 元数据只用于显示状态和启用授权操作。客户端不能把响应里的 `publisherPublicKey` 当作信任根，也不能修改、重新签名或覆盖本地制品；任何 Registry 请求链都不得推进 Document Revision 或改变 Document Hash。
- Reviewer 操作不进入 Creator 面板；Creator 只有在刷新得到 `approved` 后才能发起 publish。撤回和公钥轮换链登记留在后续切片。
- 取消会终止当前客户端等待并阻止自动创建、提交或后续刷新链继续执行，但不能回滚可能已经到达服务端的操作；取消后必须刷新状态确认最终结果。

## Packaged Player 客户端

- Player 可保留显式 ReleaseId 下载入口，也可对显式 ProjectId 调用 `GET /v1/works/{projectId}`，严格解析作品页中的最新 published ReleaseId 后再下载。P19 静态 token 模式仍只接受 `http://127.0.0.1` 或 `http://localhost`；地址、token 和独立信任公钥由当前运行入口注入，不写入配置或作品库元数据。
- 作品页响应上限 64 KiB，必须是规范 UTF-8、唯一 `application/json; charset=utf-8`、无重复/未知字段的契约 JSON；ProjectId 必须与请求一致，最新 Release 必须属于同一 Project 且状态精确为 `published`。
- Runtime 与 Creator 共用有界异步 libcurl 传输生命周期：禁止代理、重定向、netrc 和凭据跨地址转发；响应 Header 最多 64 KiB，制品最多 16 MiB。`Content-Length` 在 Header 到达时已超限会立即中止，响应压缩和 chunked artifact 均不接受。
- 下载成功必须精确包含 `application/vnd.akugc+json`、`Content-Length`、Artifact SHA-256 `ETag` 和 `X-AkUGC-Content-Hash`。客户端核对最终 URL、精确字节长度、SHA-256 和重复 Header；这些字段只绑定传输结果，不能授予作品信任。
- 信任根只能来自调用方独立提供的 64 位 Ed25519 公钥。Player 下载接口不读取 Registry 元数据里的 `publisherPublicKey`，也不接受包内公钥自证；错误公钥即使配合合法 HTTP 200、ContentHash 和 ETag 仍必须本地拒绝。
- 入库前对原始响应字节执行规范 UTF-8、`LoadVerified`、请求 ReleaseId 和已签名 ContentHash 绑定检查；全部通过后才原子创建本地 `.ugcwork`。已存在同 ID 的不同内容继续按冲突拒绝，不会覆盖。
- 选择作品时 `PrepareLibraryWork` 会从本地记录再次验签；只有准备成功后 `StartSolo` 才能建立 Session。401/403/404、重定向、超限、摘要错误、篡改制品和错误信任根都不得写入目标 Release 或启动 Session。
- Player 下载 URL 与响应格式不因 P20.4 改为直接对象地址或重定向；Registry 从内容寻址仓储读取后仍通过原端点返回，并再次核对不可变摘要和大小。服务端构建成功不能替代 Player 的本地验签。
- 当前产品切片提供 `FetchRegistryWork`、`FetchRegistryProjectWork` 与 `CancelRegistryWork` 运行时入口；Project 流程只从作品页取 ReleaseId，响应公钥不会进入本地验签参数。自动化通过仅对 Player 子进程可见的环境变量注入本机 Alpha token，避免凭据出现在 Unreal 命令行元数据。玩家账号登录、凭据安全存储与远端 TLS 地址属于正式 IdP 切片，不能沿用静态 token 方案上线公网。

## P20.1 云项目、版本与协作者

### 项目不变量

- `projectId` 由现有 Portable Project 创建，使用与签名 Manifest 相同的非零规范 UUID；服务端不能另发一个与 Document 脱节的项目 ID。
- 创建项目需要全局 `creator` 角色和 16–128 位安全 ASCII `Idempotency-Key`。同一 Owner、键、ProjectId 和 DisplayName 重试返回现有项目；同键不同元数据或同 ProjectId 不同 Owner 返回 `409`。
- `projectId`、DisplayName、唯一 Owner 和 CreatedAt 在 P20.1 创建后不可修改；协作者变化只增加 Project Revision。改名与所有权转移需要后续独立契约，不能通过直接更新数据库模拟。
- PostgreSQL 启动迁移会为 P19 已存在且 Owner 唯一的项目回填 Project；同一 ProjectId 已出现多个 Owner 时迁移失败关闭，不能猜测所有者。

### 项目角色

- Owner：创建者且唯一。可管理协作者；同时具有全局 `publisher` 时可发布或撤回项目版本。
- Editor：可读取项目和版本，并在同时具有全局 `creator`、且签名公钥登记给自身身份时提交新版本；不能发布、撤回或管理成员。
- Viewer：只能读取项目、成员和版本元数据；不能提交、审核、发布、撤回或读取未发布制品字节。
- 项目角色存储在 Registry，不由静态 token 中的全局角色替代。移除 Editor 后，即使其 Bearer token 仍有效，提交事务也必须拒绝。
- Reviewer 必须独立于项目；Owner、Editor 或 Viewer 即使同时具有全局 `reviewer` 角色，也不能审核该项目。

### 项目 HTTP v1

- `POST /v1/projects`
  - Bearer：Creator；`Content-Type: application/json`。
  - Body：`{"projectId":"<uuid>","displayName":"<1..128 UTF-8 bytes>"}`。
  - 新建返回 `201`；精确幂等重试返回 `200`。
- `GET /v1/projects/{projectId}`
  - 仅 Owner、Editor 或 Viewer；项目外主体统一返回 `404`。
- `GET /v1/projects/{projectId}/members`
  - 仅项目成员；返回 Owner 及按 Subject 排序的协作者，不返回 token 或发布私钥配置。
- `PUT /v1/projects/{projectId}/members/{subject}`
  - 仅 Owner；Body 精确为 `{"role":"editor|viewer"}`。相同角色重试不增加 Revision。
- `DELETE /v1/projects/{projectId}/members/{subject}`
  - 仅 Owner；重复删除幂等，Owner 自身不可删除。
- `GET /v1/projects/{projectId}/versions?limit=1..100&cursor={releaseId}`
  - 仅项目成员；按 SubmittedAt、ReleaseId 稳定倒序返回不含制品字节的 Release 元数据。Cursor 必须属于同一项目，未知、重复或额外查询参数均拒绝。

### 提交与并发绑定

- `POST /v1/releases` 在深度验包前先确认 Manifest ProjectId 已登记，且主体是 Owner 或 Editor；验包后在同一提交事务内再次检查，避免成员移除与提交之间的检查时使用竞态。
- PostgreSQL 提交事务对 Project 行持有共享锁；成员增删对同一 Project 行持有排他锁。因此提交要么在移除前完整授权并落库，要么在移除后明确拒绝，不存在越权中间状态。
- Release 幂等命名空间继续绑定项目 Owner，使 Owner 与 Editor 对同一稳定键和制品只产生一个版本；Editor 不能通过成为首次提交者获得发布或撤回权限。
- Project 创建使用 Owner + Idempotency-Key 事务级 advisory lock；成员变化和 Project Revision 在单一事务内更新，并追加不可变成员审计事件。

## P20.3 私人/公开作品页与发布可见性

### 可见性不变量

- Project 的 `visibility` 只能是 `private|public`。新建 Project 和迁移中的历史 Project 一律默认 `private`，不得因旧版曾允许凭 ReleaseId 下载而自动公开。
- Release 状态与 Project 可见性相互独立：`published` 表示版本可被分发，`public` 表示项目外、已认证且具有 `player` 平台角色的主体可读取作品页和下载。
- 私人 Project 的 Owner、Editor、Viewer 只有同时具有 `player` 平台角色时才能通过 Player 入口读取作品页或下载；项目外 Player 统一得到 `404`。项目管理接口的成员边界不变。
- “公开”不是匿名互联网访问。静态认证阶段所有作品页和下载仍要求有效 Player Bearer Token。

### 接口

- `PUT /v1/projects/{projectId}/visibility`
  - 仅 Owner；`Content-Type: application/json`。
  - Body 精确为 `{"visibility":"private|public"}`，重复、大小写变体和未知字段均拒绝。
  - 相同目标重试返回 `changed:false`，不增加 Revision、不追加审计；实际变化在 Project 行锁内更新 Visibility、Revision 和 UpdatedAt，并追加不可变可见性审计事件。
- `GET /v1/works/{projectId}`
  - Bearer：Player；不接受查询参数。
  - 对公开 Project，任意已认证 Player 可读；对私人 Project，仅项目成员 Player 可读。
  - 仅在至少一个 Release 当前仍为 `published` 时返回。`latestRelease` 按 SubmittedAt、ReleaseId 稳定倒序选择，不包含制品字节；最新版本撤回后，前一个仍为 published 的版本会成为当前作品页版本，全部撤回则返回 `404`。
  - 返回 `projectId`、DisplayName、Visibility、Project Revision、UpdatedAt 和完整 latest Release 元数据；不返回成员列表或 token。

### 下载与并发

- Player 对 Release 元数据读取和下载都必须同时满足可信构建 `succeeded`、published 状态以及“公开 Project 或当前项目成员”条件。构建未成功、未发布、已撤回、私人项目外访问都不能取得制品。
- PostgreSQL 可见性变化对 Project 行持有排他锁；并发相同目标只有一个请求增加 Revision 和产生审计。完成变化后的新请求按新 Visibility 判定，重叠读取允许在线性化点看到变化前或变化后的完整状态，不返回部分更新。
- 可见性切换不改变 Release、Artifact、签名、ContentHash 或发布公钥，不创建新版本，也不替代撤回操作。

## P20.4 异步可信构建与内容寻址制品

### 构建语义

- 每个不可变 Release 恰有一个构建任务，以 ReleaseId 标识；状态只允许 `queued|running|succeeded|failed`，重试复用同一任务而不是制造第二份构建身份。
- 提交请求完成外层签名、发布公钥、Project ACL 和内容寻址写入后，在同一数据库事务创建 Release 与 `queued` 构建。HTTP 请求不再等待最长 90 秒的 Unreal 深度验证。
- Worker 以有界租约领取任务，从制品仓储重新核对 SHA-256 与大小，再运行现有 `AkUGCPackTool -ValidateWork`。验证结果必须与 ReleaseId、ProjectId、Schema、ContentHash、PublisherPublicKey 和不可变制品元数据完全相同。
- 成功输出就是输入的同一 ArtifactSHA256 和 ArtifactSize，不产生重编码副本。验证拒绝为不可重试失败；验证器或制品依赖暂时不可用为可重试失败。Worker 崩溃租约最多自动回收三次，耗尽后以 `lease_expired` 可重试失败关闭。
- 构建任务保存 Attempt、Revision、输入/输出摘要、错误码及创建/开始/完成时间；租约 Owner 与截止时间只属于内部协调字段，不进入客户端 JSON。每次实际状态变化都追加不可变审计事件。

### 制品仓储与迁移

- ArtifactStore 按小写 SHA-256 内容寻址，单对象仍限制在 1..16 MiB。写入采用同目录临时文件与原子提升；已有对象必须重新读取并验证摘要，不能把同名不同字节当作成功。
- 每次读取都限制大小、要求普通文件并重新计算 SHA-256；路径只能由规范摘要导出，调用方不能提供相对路径、扩展名或任意对象键。
- PostgreSQL 模式使用显式 `AKUGC_ARTIFACT_ROOT` 的本地内容寻址实现；易失内存模拟使用内存实现。这一接口为后续远端对象存储适配建立边界，但 P20.4 不引入公网桶、预签名 URL 或客户端重定向。
- 启动迁移先把历史 `bytea` 写入并验证 ArtifactStore，再在匹配 ReleaseId、ArtifactSHA256 和原字节条件下清空数据库副本并记录迁移审计。崩溃重启可重复执行，任何写入或验证失败都保留原字节并拒绝启动。
- 迁移前的 Release 由于旧提交路径已经同步通过相同深度验证，构建任务回填为 `succeeded`；这不重新解释未登记的外部制品，也不放宽后续新提交的异步门禁。

### 权限与并发

- PostgreSQL Worker 使用行锁与 `SKIP LOCKED` 领取不同任务；同一任务只有当前租约 Owner 能完成，过期 Worker 的迟到结果不得覆盖重领后的结果。
- 并发提交仍只创建一个 Release 和一个构建任务；内容寻址写入可重复但结果必须完全相同。并发领取、完成和显式重试只能产生一条有效状态序列。
- Creator 的显式重试把 `failed + retryable` 原子恢复为 `queued` 并重置自动尝试计数；不可重试验证失败、成功任务或运行中任务不能被重置。
- 作品页选择、审核、发布和下载分别执行构建成功门禁，不能只依赖某个较早步骤已经检查过。

## 并发判定

- 提交事务先按 `Owner + Idempotency-Key` 获取事务级 advisory lock，再检查幂等记录、插入或比对 Release，最后写入幂等绑定。
- 状态迁移使用 `SELECT ... FOR UPDATE` 串行化同一 Release 的竞争操作。
- 并发相同提交只能产生一个 `201`；其余请求返回同一 Release 的 `200`。
- 并发冲突审核、重复 ID 的不同制品、复用幂等键的不同制品均必须留下明确 `409`，不能最后写入者获胜。
- 构建领取使用数据库租约和 `SKIP LOCKED`；同一租约的幂等完成可返回现有结果，不同 Worker 或不同结果的迟到完成必须冲突。

## 当前边界

- P20.4 已把新制品从 PostgreSQL `bytea` 拆到本机内容寻址 ArtifactStore，并安全迁移历史字节；当前实现仍由 Registry 进程流式返回制品，不代表已经部署 S3/云对象存储或 CDN。
- Publisher 公钥当前由本地认证配置绑定；P18.1 的双签名轮换链登记 API 是后续 P19 切片，不能通过修改现有 Release 的公钥来模拟轮换。
- P19.2 已接入 Creator Release 操作，P19.3 已接入 Packaged Player，P20.1 完成 Registry 项目权限基础，P20.2 已接入 Creator Studio 项目管理，P20.3 完成私人/公开作品页和 Player 按 Project 获取，P20.4 完成异步可信构建与内容寻址制品边界。
- Reviewer UI、撤回 UI、公开作品发现列表/搜索、正式 IdP、远端对象存储/CDN、DS 固定版本和资产包上传不属于本切片。
- 四台真实物理设备验收按当前决策延期；本阶段使用本机隔离 Owner、Editor、Viewer、Reviewer、Player 和 PostgreSQL 进程验证。

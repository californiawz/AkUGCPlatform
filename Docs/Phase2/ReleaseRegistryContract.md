# Release Registry v1 契约

> 状态：P19 最小分发闭环与 P20.1 云项目权限基础；2026-10-15。
> 范围：云项目、协作者、不可变 `.akugc` 版本的提交、审核、发布、撤回和下载。

## 信任边界

1. Registry 不签署作品，也不替 Creator 生成发布身份。
2. 上传包上限为 16 MiB。Go 入口先拒绝非法 UTF-8、深度或 token 超限 JSON、重复字段、未知包字段、非法 GUID/hex 和无效 Ed25519 Manifest 签名。
3. Go 入口必须把包内公钥绑定到已认证 Creator 的登记公钥；不能接受“包自己声明的任意可信公钥”。
4. 每次新提交还必须调用受信任的 `AkUGCCore` 深度验证器，重新检查 ContentHash、Document、Logic IR、能力白名单和签名。验证器不可用时拒绝提交，不降级为只验外层签名。
5. Registry 返回 `published` 包不代表客户端可以跳过验证。Creator、Player 和 DS 下载后仍以本地受信公钥调用现有 `FAkUGCLogicPackLoader::LoadVerified`。
6. 本地模拟使用静态 Bearer Token 摘要配置；配置只保存 SHA-256 token 摘要，服务强制只监听 loopback。该认证器不是公网账号系统，公网部署前必须替换为正式身份提供方和 TLS 终结。

## Release 不变量

- `releaseId` 是全局主键；规范形式为小写带连字符 UUID。
- `projectId`、Schema、Owner、实际提交者、PublisherPublicKey、ContentHash、ArtifactSHA256、ArtifactSize、Artifact bytes 和 SubmittedAt 创建后不可修改。
- `ownerSubject` 永远取项目 Owner，`submittedBySubject` 记录首次创建该版本的实际 Owner 或 Editor。相同制品由另一名授权协作者重试不会改写首次提交者。
- PostgreSQL 约束与触发器共同保护不可变字段；服务层不能用状态更新覆盖内容字段。
- 同一 Owner、同一 ReleaseId、同一完整制品重复提交返回现有记录，不重置状态。
- 同一 ReleaseId 的任一不可变字段或制品摘要不同，返回 `409 conflict`，不修改已有记录。
- `Idempotency-Key` 在同一 Owner 内绑定首次请求的 ArtifactSHA256；相同键和相同制品幂等，相同键和不同制品返回 `409 conflict`。

## 状态机与权限

- `submitted → approved`：只能由具备 Reviewer 角色且不是作品 Owner 的主体执行。
- `submitted → rejected`：只能由独立 Reviewer 执行，且必须提供拒绝原因；`rejected` 为终态，修改后需使用新 ReleaseId 重新提交。
- `approved → published`：只能由作品 Owner 且具备 Publisher 角色的主体执行。
- `published → revoked`：只能由作品 Owner 且具备 Publisher 角色的主体执行，且必须提供撤回原因。
- 对已处于目标状态的相同操作返回幂等成功；其他跨状态、逆向或竞争失败返回 `409 conflict`。
- 每次实际状态变化都在同一数据库事务内追加审计记录。

## HTTP v1

- `POST /v1/releases`
  - Bearer：Creator。
  - `Content-Type: application/vnd.akugc+json`。
  - 必须提供 16–128 位安全 ASCII `Idempotency-Key`。
  - 新建返回 `201`，幂等重试返回 `200`。
- `GET /v1/releases/{releaseId}`
  - Owner、Reviewer，或查看已发布版本的 Player 可访问元数据。
- `POST /v1/releases/{releaseId}/review`
  - Bearer：Reviewer。
  - JSON：`{"decision":"approved|rejected","reason":"..."}`。
- `POST /v1/releases/{releaseId}/publish`
  - Bearer：Owner + Publisher；请求体必须为空。
- `POST /v1/releases/{releaseId}/revoke`
  - Bearer：Owner + Publisher。
  - JSON：`{"reason":"..."}`。
- `GET /v1/releases/{releaseId}/download`
  - Bearer：Player。
  - 只有 `published` 返回制品；未发布、已拒绝和已撤回统一按 `404` 隐藏。
  - 响应携带 Artifact SHA-256 ETag 和 ContentHash，但二者都不能替代 Player 本地验签。

## Creator Studio 客户端

- 面板只操作最近一次成功的本地签名发布；上传前重新读取并以该记录的独立公钥、ReleaseId 和 ContentHash 执行 `LoadVerified`，不接受任意手工路径绕过。
- 静态 token 阶段仅允许 loopback HTTP；Base URL、Creator token 和请求状态只存在于面板内存，不写入项目、配置或作品库。
- 上传使用 `akugc-release-<ReleaseIdDigits>` 作为稳定幂等键。重复点击或重启后的相同包由 Registry 返回现有 Release，不创建新版本。
- submit/get/publish 响应最多 64 KiB，必须是规范 UTF-8、无重复和未知字段的契约 JSON；返回的 ReleaseId、公钥及 ContentHash 必须与本地签名记录一致。
- P20.1 响应新增必需的 `submittedBySubject`；Creator 严格解析器同步校验该字段，避免真实 Registry 与本地协议夹具漂移。
- Registry 元数据只用于显示状态和启用发布操作。客户端不能把响应里的 `publisherPublicKey` 当作信任根，也不能修改、重新签名或覆盖本地制品。
- Reviewer 操作不进入 Creator 面板；Creator 只有在刷新得到 `approved` 后才能发起 publish。撤回和公钥轮换链登记留在后续切片。
- 取消只终止客户端等待，不能回滚可能已经到达服务端的提交或发布；取消后必须刷新状态确认最终结果。

## Packaged Player 客户端

- Player 只对显式 ReleaseId 调用 `GET /v1/releases/{releaseId}/download`。P19 静态 token 模式仍只接受 `http://127.0.0.1` 或 `http://localhost`；地址、token 和独立信任公钥由当前运行入口注入，不写入配置或作品库元数据。
- Runtime 与 Creator 共用有界异步 libcurl 传输生命周期：禁止代理、重定向、netrc 和凭据跨地址转发；响应 Header 最多 64 KiB，制品最多 16 MiB。`Content-Length` 在 Header 到达时已超限会立即中止，响应压缩和 chunked artifact 均不接受。
- 下载成功必须精确包含 `application/vnd.akugc+json`、`Content-Length`、Artifact SHA-256 `ETag` 和 `X-AkUGC-Content-Hash`。客户端核对最终 URL、精确字节长度、SHA-256 和重复 Header；这些字段只绑定传输结果，不能授予作品信任。
- 信任根只能来自调用方独立提供的 64 位 Ed25519 公钥。Player 下载接口不读取 Registry 元数据里的 `publisherPublicKey`，也不接受包内公钥自证；错误公钥即使配合合法 HTTP 200、ContentHash 和 ETag 仍必须本地拒绝。
- 入库前对原始响应字节执行规范 UTF-8、`LoadVerified`、请求 ReleaseId 和已签名 ContentHash 绑定检查；全部通过后才原子创建本地 `.ugcwork`。已存在同 ID 的不同内容继续按冲突拒绝，不会覆盖。
- 选择作品时 `PrepareLibraryWork` 会从本地记录再次验签；只有准备成功后 `StartSolo` 才能建立 Session。401/403/404、重定向、超限、摘要错误、篡改制品和错误信任根都不得写入目标 Release 或启动 Session。
- 当前产品切片提供 `FetchRegistryWork` / `CancelRegistryWork` 运行时入口；自动化通过仅对 Player 子进程可见的环境变量把本机 Alpha 凭据注入该入口，避免凭据出现在 Unreal 命令行元数据。玩家账号登录、凭据安全存储与远端 TLS 地址属于正式 IdP 切片，不能沿用静态 token 方案上线公网。

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

## 并发判定

- 提交事务先按 `Owner + Idempotency-Key` 获取事务级 advisory lock，再检查幂等记录、插入或比对 Release，最后写入幂等绑定。
- 状态迁移使用 `SELECT ... FOR UPDATE` 串行化同一 Release 的竞争操作。
- 并发相同提交只能产生一个 `201`；其余请求返回同一 Release 的 `200`。
- 并发冲突审核、重复 ID 的不同制品、复用幂等键的不同制品均必须留下明确 `409`，不能最后写入者获胜。

## 当前边界

- P19 v1 将小于 16 MiB 的制品保存在 PostgreSQL `bytea`，便于先证明事务与不可变语义；对象存储拆分在 Registry 契约稳定后进行，迁移时 ArtifactSHA256 和不可变语义不得改变。
- Publisher 公钥当前由本地认证配置绑定；P18.1 的双签名轮换链登记 API 是后续 P19 切片，不能通过修改现有 Release 的公钥来模拟轮换。
- P19.2 已接入 Creator Studio，P19.3 已接入 Packaged Player；P20.1 先完成 Registry 项目/成员/版本基础与本机协议验收，Creator Studio 的项目和协作者产品界面留给下一切片。
- Reviewer UI、撤回 UI、私人/公开可见性、正式 IdP、构建任务、DS 固定版本和资产包上传不属于本切片。
- 四台真实物理设备验收按当前决策延期；本阶段使用本机隔离 Owner、Editor、Viewer、Reviewer、Player 和 PostgreSQL 进程验证。

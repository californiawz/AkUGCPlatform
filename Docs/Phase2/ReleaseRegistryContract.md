# Release Registry v1 契约

> 状态：P19 首个本地闭环；2026-10-15。
> 范围：不可变 `.akugc` Release 的提交、审核、发布、撤回和下载。

## 信任边界

1. Registry 不签署作品，也不替 Creator 生成发布身份。
2. 上传包上限为 16 MiB。Go 入口先拒绝非法 UTF-8、深度或 token 超限 JSON、重复字段、未知包字段、非法 GUID/hex 和无效 Ed25519 Manifest 签名。
3. Go 入口必须把包内公钥绑定到已认证 Creator 的登记公钥；不能接受“包自己声明的任意可信公钥”。
4. 每次新提交还必须调用受信任的 `AkUGCCore` 深度验证器，重新检查 ContentHash、Document、Logic IR、能力白名单和签名。验证器不可用时拒绝提交，不降级为只验外层签名。
5. Registry 返回 `published` 包不代表客户端可以跳过验证。Creator、Player 和 DS 下载后仍以本地受信公钥调用现有 `FAkUGCLogicPackLoader::LoadVerified`。
6. 本地模拟使用静态 Bearer Token 摘要配置；配置只保存 SHA-256 token 摘要，服务强制只监听 loopback。该认证器不是公网账号系统，公网部署前必须替换为正式身份提供方和 TLS 终结。

## Release 不变量

- `releaseId` 是全局主键；规范形式为小写带连字符 UUID。
- `projectId`、Schema、Owner、PublisherPublicKey、ContentHash、ArtifactSHA256、ArtifactSize、Artifact bytes 和 SubmittedAt 创建后不可修改。
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

## 并发判定

- 提交事务先按 `Owner + Idempotency-Key` 获取事务级 advisory lock，再检查幂等记录、插入或比对 Release，最后写入幂等绑定。
- 状态迁移使用 `SELECT ... FOR UPDATE` 串行化同一 Release 的竞争操作。
- 并发相同提交只能产生一个 `201`；其余请求返回同一 Release 的 `200`。
- 并发冲突审核、重复 ID 的不同制品、复用幂等键的不同制品均必须留下明确 `409`，不能最后写入者获胜。

## 当前边界

- P19 v1 将小于 16 MiB 的制品保存在 PostgreSQL `bytea`，便于先证明事务与不可变语义；对象存储拆分在 Registry 契约稳定后进行，迁移时 ArtifactSHA256 和不可变语义不得改变。
- Publisher 公钥当前由本地认证配置绑定；P18.1 的双签名轮换链登记 API 是后续 P19 切片，不能通过修改现有 Release 的公钥来模拟轮换。
- 私人/公开可见性、项目协作者、构建任务、DS 固定版本和资产包上传不属于本切片。
- 四台真实物理设备验收按当前决策延期；本阶段使用本机隔离 Creator、Reviewer、Player 和 PostgreSQL 进程验证。

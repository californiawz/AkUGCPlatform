# AkUGC Release Registry

P19/P20 的 Go + PostgreSQL 发布控制面。当前支持不可变 Release、云项目、
Owner/Editor/Viewer 协作者权限，以及私人/公开作品页与发布可见性。协议与安全边界见
`Docs/Phase2/ReleaseRegistryContract.md`。

## 运行

```powershell
cd Services/ReleaseRegistry
go build -o registry.exe ./cmd/registry

$env:DATABASE_URL = "postgres://akugc:password@127.0.0.1:5432/akugc?sslmode=disable"
$env:AKUGC_REGISTRY_AUTH_CONFIG = "E:/secure/registry-auth.json"
$env:AKUGC_VALIDATOR_EXE = "E:/AkStudio/UnrealEngine/Engine/Binaries/Win64/UnrealEditor-Cmd.exe"
$env:AKUGC_VALIDATOR_PROJECT = "E:/AkStudio/AkUGCPlatform/AkUGCPlatform.uproject"
$env:AKUGC_REGISTRY_ADDR = "127.0.0.1:8088"
.\registry.exe
```

服务启动时自动应用内嵌的幂等 PostgreSQL migration。没有 `DATABASE_URL` 时拒绝启动；仅本地测试可显式设置 `AKUGC_REGISTRY_MEMORY=1` 使用易失内存仓储。

## 本地认证配置

Token 明文只交给调用方，配置文件保存其 SHA-256 小写 hex：

```json
{
  "principals": [
    {
      "subject": "creator-a",
      "tokenSha256": "<64 lowercase hex>",
      "roles": ["creator", "publisher"],
      "publisherKeys": ["<Creator Ed25519 public key>"]
    },
    {
      "subject": "reviewer",
      "tokenSha256": "<64 lowercase hex>",
      "roles": ["reviewer"],
      "publisherKeys": []
    },
    {
      "subject": "player-b",
      "tokenSha256": "<64 lowercase hex>",
      "roles": ["player"],
      "publisherKeys": []
    }
  ]
}
```

静态 token 认证仅用于本地 Alpha 闭环，不可直接暴露到公网；服务会拒绝将
`AKUGC_REGISTRY_ADDR` 绑定到非 loopback 地址。

## P20.1 项目流程

已签名包中的 `manifest.projectId` 必须先由 Creator Owner 登记；Registry 不允许
任意 Creator 向未登记项目或无权限项目提交版本。

```text
POST   /v1/projects
GET    /v1/projects/{projectId}
PUT    /v1/projects/{projectId}/visibility
GET    /v1/projects/{projectId}/members
PUT    /v1/projects/{projectId}/members/{subject}
DELETE /v1/projects/{projectId}/members/{subject}
GET    /v1/projects/{projectId}/versions?limit=20&cursor={releaseId}
GET    /v1/works/{projectId}
```

- 创建项目需要 `creator` 平台角色、严格 JSON 和 16–128 位 `Idempotency-Key`。
- Owner 不可转移或删除；Owner 可幂等添加、改为 `editor|viewer` 或移除协作者。
- Editor 还必须具有 `creator` 平台角色，且包签名公钥登记在自身静态身份上，才能提交。
- Viewer 可读取项目、成员和有界版本元数据，但不能提交、审核、发布或下载未发布制品。
- Release 的 `ownerSubject` 永远绑定项目 Owner，`submittedBySubject` 记录实际提交者；
  项目成员不能审核本项目，只有 Owner + `publisher` 可发布或撤回。
- 项目外主体查询项目返回 `404`；成员移除在提交事务中再次检查，旧 token 不会绕过 ACL。

## P20.3 可见性与作品页

- Project 默认 `private`；只有 Owner 可幂等切换 `private|public`，实际变化增加 Revision 并写入可见性审计。
- `published` 控制版本是否可分发，Project Visibility 控制项目外 Player 是否可访问，两者互不替代。
- public Project 的最新 published 版本可由任意已认证 Player 通过作品页读取并下载；private Project 只允许项目成员 Player。
- 未发布、已撤回、private 项目外读取和下载统一返回 `404`。作品页与下载响应中的公钥和摘要不能替代 Player 独立信任公钥及本地验签。

## 测试

```powershell
go test ./...

# 启用真实 PostgreSQL 仓储测试：
$env:AKUGC_TEST_DATABASE_URL = "postgres://postgres@127.0.0.1:55432/postgres?sslmode=disable"
go test -count=1 ./...
```

测试默认覆盖项目与 Release 的内存并发、协作者 ACL、可见性并发幂等、私人/公开
作品页、鉴权 HTTP 生命周期、有界版本分页、Manifest 签名兼容、不可变 ID 冲突
和已撤回下载拒绝；设置数据库 URL 后额外创建隔离 Schema，验证真实事务、成员与
可见性行锁、审计、迁移默认值、状态锁、外键与数据库不可变触发器。

## 本机端到端验收

从仓库根目录执行：

```powershell
python Tools/Tests/verify_release_registry.py `
  --engine E:/AkStudio/UnrealEngine `
  --postgres-bin "C:/Program Files/PostgreSQL/16/bin"
```

驱动会创建并清理临时 PostgreSQL，启动 Registry，使用隔离的 Creator、Reviewer、
Editor、Viewer 和 Player token 完成项目登记、协作者授权与移除、并发提交、审核、
发布、私人/公开切换、作品页查询、下载和撤回；下载制品随后由
`AkUGCPackTool -ValidateWork` 与 Packaged Client 再次本地验签并完成一局。该流程是
单机角色隔离，不代表四台物理设备验收。

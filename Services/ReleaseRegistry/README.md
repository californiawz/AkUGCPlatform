# AkUGC Release Registry

P19 的 Go + PostgreSQL 最小发布控制面。协议与安全边界见 `Docs/Phase2/ReleaseRegistryContract.md`。

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

## 测试

```powershell
go test ./...

# 启用真实 PostgreSQL 仓储测试：
$env:AKUGC_TEST_DATABASE_URL = "postgres://postgres@127.0.0.1:55432/postgres?sslmode=disable"
go test -count=1 ./...
```

测试默认覆盖内存并发、鉴权 HTTP 生命周期、Manifest 签名兼容、不可变 ID 冲突和已撤回下载拒绝；设置数据库 URL 后额外创建隔离 Schema，验证真实事务、并发幂等、状态锁与数据库不可变触发器。

## 本机端到端验收

从仓库根目录执行：

```powershell
python Tools/Tests/verify_release_registry.py `
  --engine E:/AkStudio/UnrealEngine `
  --postgres-bin "C:/Program Files/PostgreSQL/16/bin"
```

驱动会创建并清理临时 PostgreSQL，启动 Registry，使用隔离的 Creator、Reviewer、
Player token 完成并发提交、审核、发布、下载和撤回；下载制品随后由
`AkUGCPackTool -ValidateWork` 与 Packaged Client 再次本地验签并完成一局。该流程是
单机角色隔离，不代表四台物理设备验收。

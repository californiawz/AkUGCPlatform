# AkLuaRuntime — 游戏无关的 Lua 运行时插件

基于 `sluaunreal` 构建的**游戏无关、可跨项目复用**的 Lua 脚本基础设施。
启用插件即获得 Lua 能力，**无需自定义 `GameInstance`、无需改 `DefaultEngine.ini`**。

---

## 设计目标

| 目标 | 实现手段 |
| --- | --- |
| 游戏无关、可复用 | 核心仅依赖 `slua_unreal`，零业务耦合；通过 `UDeveloperSettings` 暴露配置去硬编码 |
| 零侵入接入 | `UGameInstanceSubsystem` 自动绑定 GameInstance 生命周期，启用即生效 |
| 能力可插拔 | `IAkLuaBridge` + 进程级 `FAkLuaBridgeRegistry` 服务注册中心，可选能力自注册 |
| 协议解耦 | protobuf 作为独立插件 `AkLuaProtobufBridge`，核心对协议零感知 |
| 运行安全 | `FLuaCallScope` RAII 守栈；`onInitEvent` 多 VM 过滤；幂等 Shutdown |

---

## 分层架构

```
┌─────────────────────────────────────────────────────────────┐
│  接入层  ULuaScriptSubsystem (GameInstanceSubsystem)          │
│          启用即生效，转发生命周期 → Lua 入口表                │
├─────────────────────────────────────────────────────────────┤
│  核心层  FLuaVirtualMachine     VM 生命周期 / 入口调用        │
│          FLuaCallScope          栈平衡 RAII 守卫              │
│          FLuaScriptLoader       require → 磁盘字节（多根搜索）│
├─────────────────────────────────────────────────────────────┤
│  扩展层  IAkLuaBridge           能力注入契约                  │
│          FAkLuaBridgeRegistry   进程级桥接注册中心            │
│          FLuaCoreBridge         内置：print / PrintLog → 日志 │
├─────────────────────────────────────────────────────────────┤
│  配置层  ULuaRuntimeSettings    项目设置 UI 可配             │
├─────────────────────────────────────────────────────────────┤
│  底层    slua_unreal            Lua VM 提供方                 │
└─────────────────────────────────────────────────────────────┘

可选扩展插件（独立）：
  AkLuaProtobufBridge → FLuaProtobufBridge 自注册进核心注册表
                        依赖 KnightProtobuf / LuaProtobuf
```

---

## 脚本契约

入口脚本（默认 `Main`，可在项目设置改）返回一张表，下列回调全部**可选**：

| 回调 | 时机 |
| --- | --- |
| `OnInitialize(gameInstance)` | VM 启动、入口加载完成后 |
| `OnWorldBeginPlay(world)` | 世界开始游玩、可安全访问（由 `ULuaWorldSubsystem::OnWorldBeginPlay` 驱动，仅本 GameInstance 的世界；**首次调用内部触发 Initialize+GamePlay 启动流程**） |
| `OnLoadComplete()` | 地图加载完成 |
| `OnTick(dt)` | 每帧，`dt` 为秒 |
| `OnShutdown()` | GameInstance 关闭、VM 释放前 |

---

## 设计说明：`OnWorldBeginPlay` 为何选 `UWorldSubsystem::OnWorldBeginPlay`

`OnWorldBeginPlay(world)` 回调在引擎侧挂接的是
`UWorldSubsystem::OnWorldBeginPlay`（由专门的 per-world 子系统
`ULuaWorldSubsystem` 转发），而**不是**
`FWorldDelegates::OnPostWorldInitialization`，也不是
`FWorldDelegates::OnGameInstanceWorldChanged` / `UGameInstance::OnWorldChanged`。
这是一个有意的选型，原因在它们的**触发时机与世界状态保证**完全不同。

### 三者在世界加载中的真实时序

```
UEngine::LoadMap()
├─ SetCurrentWorld(nullptr)   → OnGameInstanceWorldChanged(GI, Old, null)   ① 拆旧世界
├─ NewWorld->SetGameInstance(GI)
├─ SetCurrentWorld(NewWorld)  → OnGameInstanceWorldChanged(GI, null, New)   ② 新世界“尚未初始化”
├─ NewWorld->InitWorld()      → OnPostWorldInitialization(New, IVS)         ③ 新世界已完整初始化
└─ NewWorld->BeginPlay()      → UWorldSubsystem::OnWorldBeginPlay(World)    ④ 世界开始游玩
```

①② 时新世界还是空壳（无 Scene、PersistentLevel 未初始化）；③ 时世界已完成
`InitWorld()` 但 Actor 尚未 `BeginPlay`；④ 才是“游玩真正开始”：Actor 已跑过
`BeginPlay`、视口就绪，是比 ③ 更强的可用性保证。

### 差异对比

| 维度 | `OnGameInstanceWorldChanged` | `OnPostWorldInitialization` | `OnWorldBeginPlay`（本插件采用） |
| --- | --- | --- | --- |
| 时序 | 最早（`SetCurrentWorld`） | 居中（`InitWorld`） | 最晚（`BeginPlay`） |
| 新世界状态 | **未初始化** | **已初始化**，Actor 未 BeginPlay | **已开始游玩**，Actor 已 BeginPlay |
| 触发次数 | 一次切换触发多次（旧→null、null→新） | 每个世界一次 | 每个游戏世界一次（仅 Game/PIE） |
| 携带信息 | 旧、新两端（任一可为 null） | 单个新世界 + 初始化参数 | 单个已开始游玩的世界 |

### 结论

脚本层 `OnWorldBeginPlay(world)` 的语义是“**进入一个已开始游玩、Actor 已就绪
的世界**”，因此取最强保证的 `UWorldSubsystem::OnWorldBeginPlay` 时机。该回调由
`ULuaWorldSubsystem`（per-world 子系统，仅在 Game/PIE 世界创建）转发至
GameInstance 级的 `ULuaScriptSubsystem::DispatchWorldBeginPlay`，后者持有 VM 并将
首次 `OnWorldBeginPlay` 作为 GamePlay 启动点。

> 若未来确有“新旧世界对比 / 清理旧世界引用”的需求，可在核心另加一个
> 基于 `OnGameInstanceWorldChanged` 的独立回调（如 `OnWorldTransition(old, new)`），
> 与现有 `OnWorldBeginPlay` 正交并存，而非替换它。

---

## 脚本搜索优先级（由配置驱动）

1. `PersistentDownloadDir/<Root>/` — 热更/补丁脚本最高优先
2. `../<Root>/`（编辑器，项目同级）— 免 cook 热改
3. `ProjectContentDir/<Root>/` — 打包后脚本

`<Root>` 默认 `Lua`，扩展名默认 `.lua` / `.luac`，均可在项目设置调整。

---

## 在新游戏中复用

1. 拷贝 `AkLuaRuntime`（及可选 `AkLuaProtobufBridge`）到目标工程 `Plugins/`。
2. `.uproject` 启用插件（protobuf 桥接还需 `slua_unreal` + `KnightProtobuf`）。
3. 在脚本根目录放 `Main.lua`，实现需要的回调。
4. 完成。无需自定义 GameInstance，无需改引擎配置。

---

## 扩展新能力（自定义桥接）

```cpp
class FMyBridge : public IAkLuaBridge
{
public:
    static const FName BridgeName; // TEXT("MyFeature")
    virtual FName GetBridgeName() const override { return BridgeName; }
    virtual void Install(NS_SLUA::lua_State* L) override { /* 注册到 L */ }
};

// 在模块 StartupModule 中：
FAkLuaBridgeRegistry::Get().Register(
    FMyBridge::BridgeName,
    []() -> TSharedRef<IAkLuaBridge> { return MakeShared<FMyBridge>(); });
```

核心会在每个 VM 启动时自动安装所有已注册桥接，**无需改动核心代码**。

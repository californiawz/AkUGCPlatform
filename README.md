# AkUGCPlatform

AkStudio 的 Unreal Engine UGC 创作与运行平台。

## 当前阶段

Phase 0：技术风险验证。当前优先验证统一内容模型、Prefab、Command、Logic IR、沙箱、Logic Pack，以及 Win64/Android/DS 一致运行。

首个垂直切片为“三波合作塔防”。首期只使用官方 Prefab，不开放玩家 C++、插件、任意 Blueprint、任意 UObject 反射或外部网络访问。

## 技术基线

- Unreal Engine 5.8.1：`E:/AkStudio/UnrealEngine`
- Creator：Win64 Editor
- Player：Win64，随后 Android
- Authority：Win64 Dedicated Server
- 脚本：Lua 5.3.4；玩家脚本必须使用独立沙箱
- 后端：Go + PostgreSQL，云端控制面延后到本地闭环完成后

## 目录

```text
Build/                  项目构建配置
Config/                 Unreal 项目配置
Docs/ADR/               架构决策记录
Docs/Phase0/            Phase 0 范围与验收
Plugins/AkUGCCore/      共享 ID、Manifest、Schema 与版本契约
Source/AkUGCPlatform/   最小项目宿主模块
```

## 生成项目文件

```powershell
E:/AkStudio/UnrealEngine/GenerateProjectFiles.bat -project="E:/AkStudio/AkUGCPlatform/AkUGCPlatform.uproject" -game -engine
```

## 编译 Editor

```powershell
E:/AkStudio/UnrealEngine/Engine/Build/BatchFiles/Build.bat AkUGCPlatformEditor Win64 Development "E:/AkStudio/AkUGCPlatform/AkUGCPlatform.uproject" -WaitMutex -FromMsBuild
```

## 构建预演

```powershell
E:/AkStudio/AkUGCPlatform/Build/Build_Win64.bat --list-steps
E:/AkStudio/AkUGCPlatform/Build/Build_Win64.bat --dry-run
```

## 不可破坏的原则

1. `UGC Project Document` 是 Portable 项目的唯一真源。
2. Desktop、App、AI 必须共用 Document、PrefabId、Command、Validator 和 Logic IR。
3. `.umap` 是预览或编译产物，不是 Portable 项目的唯一真源。
4. 平台业务 Lua 与玩家 UGC Lua 必须使用不同 VM 和权限边界。
5. 发布版本不可变，运行端只接受兼容且签名可信的 Release。

# ADR-0003：玩家 UGC 使用独立 Lua 沙箱

- 状态：Accepted
- 日期：2026-09-03

## 决策

平台可信业务 Lua 与玩家 UGC Lua 使用不同的 `lua_State`、加载器、标准库和宿主 API。现有 `AkLuaRuntime/slua_unreal` 仅可作为可信业务基础或实现参考，不直接作为陌生玩家脚本的安全边界。

UGC VM 禁止 UObject 通用反射、`import`、LuaSocket、文件 IO、操作系统调用、Debug 库、动态原生模块和任意本地文件加载。宿主能力通过版本化 Capability API 暴露，并实施内存、指令、时间、任务、事件和实体配额。

## 原因

现有可信热更新 VM 可以从下载目录加载脚本并访问 UE 反射，其权限远高于公开 UGC 所能接受的范围。

## 影响

L3 Lua 在沙箱完成安全门禁前不得公开。L1 模板和 L2 Trigger Graph 也必须经过同一 Capability、Effect 和 Budget Validator。

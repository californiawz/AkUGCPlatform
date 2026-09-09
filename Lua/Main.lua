---------------------------------------------------------------------
-- AkUGCPlatform (C) Akatsuki, All Rights Reserved
-- Desc: Lua 入口文件。由游戏无关插件 AkLuaRuntime 的 ULuaScriptSubsystem
--       在 GameInstance 初始化时 doFile("Main")，并把本表作为回调环境，
--       依次驱动生命周期（均为可选，C++ 缺省静默跳过）：
--         OnInitialize(gameInstance) -- VM 启动、入口加载完成后；传入 GameInstance
--         OnWorldBeginPlay(world)    -- 世界开始游玩，可安全访问关卡/Actor
--         OnLoadComplete(world)      -- 地图加载完成
--         OnTick(dt)                 -- 每帧，dt 为秒
--         OnShutdown()               -- GameInstance 关闭、VM 释放前
--
--   当前由 AkLuaRuntime Core Bridge 注入的全局：
--     日志：PrintLog / print / PrintVerbose / PrintDisplay / PrintWarning / PrintError
--     平台：TW_PLATFORM (string) / TW_IS_EDITOR (bool) / TW_IS_MOBILE (bool)
--
--   说明：AkUGCPlatform 的玩法逻辑（塔防 UGC）当前由 C++ 实现
--         （AkUGCCore / AAkUGCGameMode），Lua 层作为可扩展入口预留，
--         后续可在 require 的模块中接入业务脚本。
---------------------------------------------------------------------
local M = {}

local bGameplayStarted = false

---VM 启动、入口脚本加载完成后调用一次。
---@param gameInstance any UGameInstance
function M.OnInitialize(gameInstance)
    PrintLog(string.format(
        "[AkUGCPlatform] Lua VM initialized | platform=%s editor=%s mobile=%s",
        tostring(TW_PLATFORM), tostring(TW_IS_EDITOR), tostring(TW_IS_MOBILE)))
end

---世界开始游玩（可安全访问关卡/Actor）。
---@param world UWorld
function M.OnWorldBeginPlay(world)
    if not bGameplayStarted then
        bGameplayStarted = true
        PrintLog("[AkUGCPlatform] gameplay started")
    end
end

---地图加载完成。
---@param world UWorld
function M.OnLoadComplete(world)
    PrintLog("[AkUGCPlatform] map load complete")
end

---帧更新，dt 为秒。
---@param dt number
function M.OnTick(dt)
    -- 预留：逐帧逻辑
end

---GameInstance 关闭、VM 释放前调用一次。
function M.OnShutdown()
    PrintLog("[AkUGCPlatform] Lua VM shutdown")
end

return M

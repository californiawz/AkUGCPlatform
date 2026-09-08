// Copyright Akatsuki. All Rights Reserved.

#include "Bridge/LuaCoreBridge.h"
#include "AkLuaRuntimeModule.h"
#include "LuaState.h"
#include "GenericPlatform/GenericPlatformProperties.h"

using namespace NS_SLUA;

const FName FLuaCoreBridge::BridgeName(TEXT("Core"));

// Concatenate all arguments (tab-separated, lua-style) into a single FString.
static FString AkLua_JoinArgs(lua_State* L)
{
	const int32 ArgCount = lua_gettop(L);
	FString Line;
	for (int32 i = 1; i <= ArgCount; ++i)
	{
		size_t Len = 0;
		const char* Str = luaL_tolstring(L, i, &Len); // pushes a string copy onto the stack
		if (Str)
		{
			if (i > 1)
			{
				Line += TEXT("\t");
			}
			Line += UTF8_TO_TCHAR(Str);
		}
		lua_pop(L, 1); // balance the value pushed by luaL_tolstring
	}
	return Line;
}

// Verbosity is fixed per registered global, so UE_LOG receives a compile-time
// constant (it does not accept a runtime ELogVerbosity value).
static int32 AkLua_PrintLog(lua_State* L)
{
	UE_LOG(LogAkLua, Log, TEXT("[Lua] %s"), *AkLua_JoinArgs(L));
	return 0;
}

static int32 AkLua_PrintDisplay(lua_State* L)
{
	UE_LOG(LogAkLua, Display, TEXT("[Lua] %s"), *AkLua_JoinArgs(L));
	return 0;
}

static int32 AkLua_PrintWarning(lua_State* L)
{
	UE_LOG(LogAkLua, Warning, TEXT("[Lua] %s"), *AkLua_JoinArgs(L));
	return 0;
}

static int32 AkLua_PrintError(lua_State* L)
{
	UE_LOG(LogAkLua, Error, TEXT("[Lua] %s"), *AkLua_JoinArgs(L));
	return 0;
}

static int32 AkLua_PrintVerbose(lua_State* L)
{
	UE_LOG(LogAkLua, Verbose, TEXT("[Lua] %s"), *AkLua_JoinArgs(L));
	return 0;
}

// Push platform facts as globals so the Lua Native adapter can read them
// without a reflection call. Mirrors the contract in Framework/Engine/Native.lua:
//   TW_PLATFORM (string) / TW_IS_EDITOR (bool) / TW_IS_MOBILE (bool)
static void InstallPlatformConstants(lua_State* L)
{
	const FString PlatformName(FPlatformProperties::IniPlatformName());
	lua_pushstring(L, TCHAR_TO_UTF8(*PlatformName));
	lua_setglobal(L, "TW_PLATFORM");

#if WITH_EDITOR
	lua_pushboolean(L, 1);
#else
	lua_pushboolean(L, 0);
#endif
	lua_setglobal(L, "TW_IS_EDITOR");

#if PLATFORM_IOS || PLATFORM_ANDROID
	lua_pushboolean(L, 1);
#else
	lua_pushboolean(L, 0);
#endif
	lua_setglobal(L, "TW_IS_MOBILE");
}

void FLuaCoreBridge::Install(lua_State* L)
{
	// 默认日志（Log 级）：保留既有全局，兼容历史调用。
	lua_pushcfunction(L, AkLua_PrintLog);
	lua_setglobal(L, "PrintLog");

	lua_pushcfunction(L, AkLua_PrintLog);
	lua_setglobal(L, "print");

	// 按级别拆分的全局，供 Lua Logger 按 level 分发到不同 verbosity。
	lua_pushcfunction(L, AkLua_PrintVerbose);
	lua_setglobal(L, "PrintVerbose");

	lua_pushcfunction(L, AkLua_PrintDisplay);
	lua_setglobal(L, "PrintDisplay");

	lua_pushcfunction(L, AkLua_PrintWarning);
	lua_setglobal(L, "PrintWarning");

	lua_pushcfunction(L, AkLua_PrintError);
	lua_setglobal(L, "PrintError");

	InstallPlatformConstants(L);
}

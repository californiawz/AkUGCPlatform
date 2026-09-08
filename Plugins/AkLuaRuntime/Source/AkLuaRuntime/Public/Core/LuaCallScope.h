// Copyright Akatsuki. All Rights Reserved.

#pragma once

#include "LuaState.h"

/**
 * FLuaCallScope
 *
 * RAII guard that snapshots the Lua stack top on construction and restores it
 * on destruction. Guarantees stack balance around a native->Lua call even if
 * the callee leaves values (or errors) on the stack, preventing slow stack
 * leaks that are notoriously hard to diagnose.
 */
struct FLuaCallScope
{
	explicit FLuaCallScope(NS_SLUA::lua_State* InL)
		: L(InL)
		, SavedTop(InL ? NS_SLUA::lua_gettop(InL) : 0)
	{
	}

	~FLuaCallScope()
	{
		if (L)
		{
			NS_SLUA::lua_settop(L, SavedTop);
		}
	}

	FLuaCallScope(const FLuaCallScope&) = delete;
	FLuaCallScope& operator=(const FLuaCallScope&) = delete;

	NS_SLUA::lua_State* L;
	int SavedTop;
};

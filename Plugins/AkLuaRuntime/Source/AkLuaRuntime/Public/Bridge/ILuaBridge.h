// Copyright Akatsuki. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

// Forward-declare the slua lua_State without dragging slua headers into this
// interface header, keeping the bridge abstraction lightweight.
namespace NS_SLUA { struct lua_State; }

/**
 * IAkLuaBridge
 *
 * A bridge injects native (C++) capabilities into a freshly created Lua state:
 * global functions, libraries, metatables, third-party VM extensions, etc.
 *
 * Bridges are the extension seam of the runtime. The core ships a logging
 * bridge; optional features (protobuf, networking, ...) provide their own
 * bridges from separate plugins, so the core stays dependency-free.
 *
 * Install() is invoked exactly once per VM, during LuaState initialization.
 */
class IAkLuaBridge
{
public:
	virtual ~IAkLuaBridge() = default;

	/** Stable identifier, also used as the registry key. */
	virtual FName GetBridgeName() const = 0;

	/** Register this bridge's capabilities onto the given lua_State. */
	virtual void Install(NS_SLUA::lua_State* L) = 0;

	/** Optional symmetric teardown, called before the VM closes. */
	virtual void Uninstall(NS_SLUA::lua_State* /*L*/) {}
};

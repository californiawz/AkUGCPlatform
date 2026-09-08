// Copyright Akatsuki. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Bridge/ILuaBridge.h"

/**
 * FLuaCoreBridge
 *
 * Always-on bridge shipped by the runtime core. Routes Lua `print` / `PrintLog`
 * to the engine log, giving scripts a usable output channel out of the box.
 */
class FLuaCoreBridge : public IAkLuaBridge
{
public:
	static const FName BridgeName;

	virtual FName GetBridgeName() const override { return BridgeName; }
	virtual void Install(NS_SLUA::lua_State* L) override;
};

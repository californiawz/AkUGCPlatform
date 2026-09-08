#include "LuaActor.h"
#include "LuaState.h"
#include "Net/UnrealNetwork.h"

ALuaActor::ALuaActor(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
}

void ALuaActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    Super::EndPlay(EndPlayReason);

    if (EnableLuaTick)
    {
        UnRegistLuaTick();
    }
}

FString ALuaActor::GetLuaFilePath_Implementation() const
{
    return LuaFilePath;
}

void ALuaActor::PostInitializeComponents()
{
    Super::PostInitializeComponents();
    ILuaOverriderInterface::PostLuaHook();
}

void ALuaActor::RegistLuaTick(float TickInterval)
{
    EnableLuaTick = true;
    auto state = NS_SLUA::LuaState::get();
    state->registLuaTick(this, TickInterval);
}

void ALuaActor::UnRegistLuaTick()
{
    auto state = NS_SLUA::LuaState::get();
    state->unRegistLuaTick(this);
}

void ALuaActor::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    
    {
        DOREPLIFETIME_CONDITION(ALuaActor, LuaNetSerialization, COND_None);
    }
}

NS_SLUA::LuaVar  ALuaActor::callMember(FString func, const TArray<FLuaBPVar>& args)
{
    NS_SLUA::LuaVar lfunc = GetSelfTable().getFromTable<NS_SLUA::LuaVar>((const char*)TCHAR_TO_UTF8(*func));
    if (!lfunc.isFunction()) {
        NS_SLUA::Log::Error("Can't find lua member function named %s to call", TCHAR_TO_UTF8(*func));
        return false;
    }
 
    auto L = GetSelfTable().getState();

    // 修改fillParam，将self和args都压入
    auto fillParam = [L, this, &args]()
        {
            // 先压入self
            GetSelfTable().push(L);
            // 再压入args
            for (auto& arg : args) {
                arg.value.push(L);
            }
            return args.Num() + 1; // 包括self
        };

    NS_SLUA::LuaVar var = lfunc.callWithNArg(fillParam);

   return var;

}
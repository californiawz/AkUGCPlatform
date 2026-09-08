#include "Validation/AkUGCCapabilityValidator.h"

TArray<FName> FAkUGCCapabilityValidator::GetSupportedCapabilities()
{
	// Phase 0 三波合作塔防切片支持的运行时能力。
	return {
		TEXT("logic"),        // L2 Trigger Graph（GameStart/Message/Timer/Spawn/WaveStart）。
		TEXT("ruleset"),      // 塔防 Ruleset（三波状态机）。
		TEXT("world.spawn"),  // 世界刷怪能力（官方 Prefab 生成）。
		TEXT("rules.wave"),   // 波次规则（波次配置与触发）。
	};
}

bool FAkUGCCapabilityValidator::Validate(const TArray<FName>& Capabilities, FString* OutError)
{
	const TArray<FName> Supported = GetSupportedCapabilities();
	for (const FName& Capability : Capabilities)
	{
		if (!Supported.Contains(Capability))
		{
			if (OutError)
			{
				*OutError = FString::Printf(
					TEXT("作品声明了运行时不支持的能力：%s。"),
					*Capability.ToString());
			}
			return false;
		}
	}
	return true;
}

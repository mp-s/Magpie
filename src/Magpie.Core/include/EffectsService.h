#pragma once
#include <parallel_hashmap/phmap.h>
#include "EffectInfo.h"

namespace Magpie {

class EffectsService {
public:
	static EffectsService& Get() noexcept {
		static EffectsService instance;
		return instance;
	}

	EffectsService(const EffectsService&) = delete;
	EffectsService(EffectsService&&) = delete;

	winrt::fire_and_forget Initialize();

	void Uninitialize();

	const std::vector<EffectInfo>& GetEffects() noexcept;

	// 由于 WinUI 使用 UTF-16，这里也以 UTF-16 作为参数以减少编码转换
	const EffectInfo* GetEffect(std::wstring_view name) noexcept;

private:
	EffectsService() = default;

	void _WaitForInitialize() noexcept;

	std::vector<EffectInfo> _effects;
	phmap::flat_hash_map<std::wstring, uint32_t> _effectsMap;
	std::atomic<bool> _initialized = false;
	bool _initializedCache = false;
};

}

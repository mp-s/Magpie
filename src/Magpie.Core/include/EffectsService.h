#pragma once
#include "EffectInfo.h"
#include "../ShaderEffectDesc.h"
#include <parallel_hashmap/phmap.h>

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

	const EffectInfo* GetEffect(std::string_view name) noexcept;

	std::string SubmitCompileShaderEffectTask(
		std::string_view effectName,
		const phmap::flat_hash_map<std::string, float>* inlineParams,
		D3D_SHADER_MODEL shaderModel,
		bool isFP16Supported,
		bool isAdvancedColorSupported
	) noexcept;

	bool GetTaskResult(std::string taskKey, const ShaderEffectDesc* &effectDesc) noexcept;

	// 缩放结束时调用
	void CleanCache(bool clearAll) noexcept;

private:
	EffectsService() = default;

	void _WaitForInitialize() noexcept;

	std::vector<EffectInfo> _effects;
	phmap::flat_hash_map<std::string_view, uint32_t> _effectsMap;

	struct _ShaderEffectMemCacheItem {
		ShaderEffectDesc effectDesc;
		// UINT_MAX 表示尚未编译完成
		uint32_t lastAccess = std::numeric_limits<uint32_t>::max();
	};
	phmap::flat_hash_map<std::string, _ShaderEffectMemCacheItem> _shaderEffectCache;
	
	std::atomic<bool> _initialized = false;
	bool _initializedCache = false;
};

}

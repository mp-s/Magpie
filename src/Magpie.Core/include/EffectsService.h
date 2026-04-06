#pragma once
#include "EffectInfo.h"
#include "../ShaderEffectDrawInfo.h"
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
		bool isMinFloat16Supported,
		bool isNative16BitSupported,
		bool isAdvancedColorSupported,
		bool saveSources,
		bool warningsAreErrors
	) noexcept;

	bool GetTaskResult(const std::string& taskKey, const ShaderEffectDrawInfo** drawInfo) noexcept;

	void ReleaseTask(const std::string& taskKey) noexcept;

private:
	EffectsService() = default;

	void _WaitForInitialize() noexcept;

	winrt::fire_and_forget _CompileShaderEffectAsync(
		std::string effectName,
		std::string source,
		const phmap::flat_hash_map<std::string, float>* inlineParams,
		D3D_SHADER_MODEL shaderModel,
		std::string cacheKey,
		uint32_t parserFlags,
		bool saveSources,
		bool warningsAreErrors
	) noexcept;

	std::vector<EffectInfo> _effects;
	phmap::flat_hash_map<std::string_view, uint32_t> _effectsMap;

	struct _ShaderEffectMemCacheItem {
		ShaderEffectDrawInfo drawInfo;
		// UINT_MAX 表示正在使用中
		uint32_t lastAccess = std::numeric_limits<uint32_t>::max();
	};
	// 需确保 drawInfo 地址稳定
	phmap::node_hash_map<std::string, _ShaderEffectMemCacheItem> _shaderEffectCache;
	wil::srwlock _shaderEffectCacheLock;
	uint32_t _nextLastAccess = 0;
	
	std::atomic<bool> _initialized = false;
	bool _initializedCache = false;
};

}

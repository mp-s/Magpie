#include "pch.h"
#include "CommonSharedConstants.h"
#include "ShaderEffectParser.h"
#include "EffectsService.h"
#include "Logger.h"
#include "StrHelper.h"
#include "Win32Helper.h"
#include <rapidhash.h>

namespace Magpie {

static void ListEffects(std::vector<std::wstring>& result, std::wstring_view prefix = {}) {
	result.reserve(80);

	WIN32_FIND_DATA findData{};
	wil::unique_hfind hFind(FindFirstFileEx(
		StrHelper::Concat(CommonSharedConstants::EFFECTS_DIR, L"\\", prefix, L"*").c_str(),
		FindExInfoBasic, &findData, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH));
	if (!hFind) {
		Logger::Get().Win32Error("FindFirstFileEx 失败");
		return;
	}

	do {
		std::wstring_view fileName(findData.cFileName);
		if (fileName == L"." || fileName == L"..") {
			continue;
		}

		if (Win32Helper::DirExists(StrHelper::Concat(
			CommonSharedConstants::EFFECTS_DIR, L"\\", prefix, fileName).c_str())) {
			ListEffects(result, StrHelper::Concat(prefix, fileName, L"\\"));
			continue;
		}

		if (!fileName.ends_with(L".hlsl")) {
			continue;
		}

		result.emplace_back(StrHelper::Concat(prefix, fileName.substr(0, fileName.size() - 5)));
	} while (FindNextFile(hFind.get(), &findData));
}

winrt::fire_and_forget EffectsService::Initialize() {
	co_await winrt::resume_background();

	std::vector<std::wstring> effectNames;
	ListEffects(effectNames);

	const uint32_t nEffect = (uint32_t)effectNames.size();
	_effectsMap.reserve(nEffect);
	_effects.reserve(nEffect);

	// 用于同步 _effectsMap 和 _effects 的初始化
	wil::srwlock srwLock;

	// 并行解析效果
	Win32Helper::RunParallel([&](uint32_t id) {
		std::wstring fileName = StrHelper::Concat(
			CommonSharedConstants::EFFECTS_DIR, L"\\", effectNames[id], L".hlsl");
		std::string source;
		Win32Helper::ReadTextFile(fileName.c_str(), source);
		EffectInfo effectInfo;
		std::string errorMsg = ShaderEffectParser::ParseForInfo(
			StrHelper::UTF16ToUTF8(effectNames[id]), std::move(source), effectInfo);
		if (!errorMsg.empty()) {
			return;
		}
		
		auto lock = srwLock.lock_exclusive();
		uint32_t effectIdx = (uint32_t)_effects.size();
		EffectInfo& movedEffectInfo = _effects.emplace_back(std::move(effectInfo));
		_effectsMap.emplace(movedEffectInfo.name, effectIdx);
	}, nEffect);

	_initialized.store(true, std::memory_order_release);
	_initialized.notify_one();
}

void EffectsService::Uninitialize() {
	// 等待解析完成，防止退出时崩溃
	_WaitForInitialize();
}

const std::vector<EffectInfo>& EffectsService::GetEffects() noexcept {
	_WaitForInitialize();
	return _effects;
}

const EffectInfo* EffectsService::GetEffect(std::string_view name) noexcept {
	_WaitForInitialize();

	auto it = _effectsMap.find(name);
	return it != _effectsMap.end() ? &_effects[it->second] : nullptr;
}

static std::string GetLinearEffectName(std::string_view effectName) {
	std::string result(effectName);
	for (char& c : result) {
		if (c == '\\') {
			c = '#';
		}
	}
	return result;
}

static std::string GetCacheFileName(
	std::string_view linearEffectName,
	D3D_SHADER_MODEL shaderModel,
	ShaderEffectParserFlags flags,
	uint64_t hash
) {
	// 缓存文件的命名: {效果名}_{shader model(2)}{标志位(4)}{哈希(16)）}
	return fmt::format("{}\\{}_{:02x}{:04x}{:016x}",
		StrHelper::UTF16ToUTF8(CommonSharedConstants::CACHE_DIR),
		linearEffectName, (uint16_t)shaderModel, (uint32_t)flags, hash);
}

std::string EffectsService::SubmitCompileShaderEffectTask(
	std::string_view effectName,
	const phmap::flat_hash_map<std::string, float>* inlineParams,
	D3D_SHADER_MODEL shaderModel,
	bool isFP16Supported,
	bool isAdvancedColorSupported
) noexcept {
	_WaitForInitialize();

	std::string cacheKey;

	auto it = _effectsMap.find(effectName);
	if (it == _effectsMap.end()) {
		return cacheKey;
	}
	const EffectInfo& effectInfo = _effects[it->second];

	std::string source;
	{
		std::wstring fileName = StrHelper::Concat(
			CommonSharedConstants::EFFECTS_DIR, L"\\", StrHelper::UTF8ToUTF16(effectName), L".hlsl");
		if (!Win32Helper::ReadTextFile(fileName.c_str(), source)) {
			return cacheKey;
		}
	}

	ShaderEffectParserOptions options = {
		.inlineParams = inlineParams,
		.shaderModel = shaderModel
	};
	if (isFP16Supported && bool(effectInfo.flags & EffectInfoFlags::SupportFP16)) {
		options.flags |= ShaderEffectParserFlags::EnableFP16;
	}
	if (isAdvancedColorSupported && bool(effectInfo.flags & EffectInfoFlags::SupportAdvancedColor)) {
		options.flags |= ShaderEffectParserFlags::EnableAdvancedColor;
	}
	
	// shaderModel 和 flags 不参与哈希，它们决定缓存键（也是缓存文件名）
	uint64_t hash = rapidhash(source.data(), source.size());
	if (inlineParams) {
		for (const auto& pair : *inlineParams) {
			for (const EffectInfoParameter& param : effectInfo.params) {
				if (param.name == pair.first) {
					// 将参数值归一化然后保留 4 位精度
					long normValue = std::lroundf((pair.second - param.minValue) /
						(param.maxValue - param.minValue) * 10000);
					hash = phmap::HashState().combine(hash, pair.first, normValue);
					break;
				}
			}
		}
	}
	
	cacheKey = GetCacheFileName(
		GetLinearEffectName(effectName), shaderModel, options.flags, hash);

	if (!_shaderEffectCache.contains(cacheKey)) {
		// _shaderEffectCache.emplace(cacheKey, _ShaderEffectMemCacheItem{});

		ShaderEffectDrawInfo effectDesc;
		ShaderEffectSource effectSource;
		std::string errorMsg = ShaderEffectParser::ParseForDesc(
			effectInfo,
			std::move(source),
			options,
			effectDesc,
			effectSource
		);
		if (!errorMsg.empty()) {
			// 解析失败
			_shaderEffectCache.erase(cacheKey);
			cacheKey.clear();
			return cacheKey;
		}
	}

	return cacheKey;
}

bool EffectsService::GetTaskResult(std::string taskKey, const ShaderEffectDrawInfo** effectDesc) noexcept {
	auto it = _shaderEffectCache.find(taskKey);
	if (it == _shaderEffectCache.end()) {
		// 编译失败
		return false;
	}

	if (it->second.lastAccess == std::numeric_limits<uint32_t>::max()) {
		// 尚未编译完成
		*effectDesc = nullptr;
		return true;
	}

	*effectDesc = &it->second.effectDesc;
	return true;
}

void EffectsService::CleanCache(bool /*clearAll*/) noexcept {
}

void EffectsService::_WaitForInitialize() noexcept {
	if (_initializedCache) {
		return;
	}

	_initialized.wait(false, std::memory_order_acquire);
	_initializedCache = true;
}

}

#include "pch.h"
#include "CommonSharedConstants.h"
#include "EffectsService.h"
#include "Logger.h"
#include "ShaderEffectParser.h"
#include "StrHelper.h"
#include "Win32Helper.h"
#include <d3dcompiler.h>
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
	bool isAdvancedColorSupported,
	bool saveSources,
	bool warningsAreErrors
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

	ShaderEffectParserFlags parserFlags = ShaderEffectParserFlags::None;
	if (isFP16Supported && bool(effectInfo.flags & EffectFlags::SupportFP16)) {
		parserFlags |= ShaderEffectParserFlags::EnableFP16;
	}
	if (isAdvancedColorSupported && bool(effectInfo.flags & EffectFlags::SupportAdvancedColor)) {
		parserFlags |= ShaderEffectParserFlags::EnableAdvancedColor;
	}
	
	// shaderModel 和 flags 不参与哈希，它们决定缓存键（也是缓存文件名）
	uint64_t hash = rapidhash(source.data(), source.size());
	if (inlineParams) {
		// 即使 inlineParams 中不包含的参数也参与哈希，否则无法区分未启用内联变量和
		// 启用但 inlineParams 中没有成员。
		for (const EffectParameterDesc& param : effectInfo.params) {
			float value;
			auto it1 = inlineParams->find(param.name);
			if (it1 != inlineParams->end()) {
				value = it1->second;
			} else {
				value = param.defaultValue;
			}

			// 将参数值归一化然后保留 4 位精度
			long normValue = std::lround((value - param.minValue) /
				(param.maxValue - param.minValue) * 10000);
			hash = phmap::HashState().combine(hash, normValue);
		}
	}
	
	cacheKey = GetCacheFileName(
		GetLinearEffectName(effectName), shaderModel, parserFlags, hash);

	{
		auto lk = _shaderEffectCacheLock.lock_exclusive();

		if (_shaderEffectCache.contains(cacheKey)) {
			return cacheKey;
		}

		_shaderEffectCache.emplace(cacheKey, _ShaderEffectMemCacheItem{});
	}
	
	_CompileShaderEffectAsync(
		std::string(effectName),
		std::move(source),
		inlineParams,
		shaderModel,
		cacheKey,
		isFP16Supported,
		isAdvancedColorSupported,
		saveSources,
		warningsAreErrors
	);
	
	return cacheKey;
}

bool EffectsService::GetTaskResult(
	const std::string& taskKey,
	const ShaderEffectDrawInfo** drawInfo
) noexcept {
	auto lk = _shaderEffectCacheLock.lock_shared();

	auto it = _shaderEffectCache.find(taskKey);
	if (it == _shaderEffectCache.end()) {
		// 编译失败
		return false;
	}

	if (it->second.drawInfo.passes.empty()) {
		// 尚未编译完成
		*drawInfo = nullptr;
		return true;
	}

	*drawInfo = &it->second.drawInfo;
	return true;
}

void EffectsService::ReleaseTask(const std::string& taskKey) noexcept {
	auto lk = _shaderEffectCacheLock.lock_exclusive();

	auto it = _shaderEffectCache.find(taskKey);
	if (it == _shaderEffectCache.end()) {
		return;
	}

	it->second.lastAccess = _nextLastAccess++;
}

void EffectsService::_WaitForInitialize() noexcept {
	if (_initializedCache) {
		return;
	}

	_initialized.wait(false, std::memory_order_acquire);
	_initializedCache = true;
}

class FXCInclude : public ID3DInclude {
public:
	FXCInclude(std::filesystem::path&& localDir) : _localDir(std::move(localDir)) {}

	FXCInclude(const FXCInclude&) = default;
	FXCInclude(FXCInclude&&) = default;

	HRESULT CALLBACK Open(
		D3D_INCLUDE_TYPE /*IncludeType*/,
		LPCSTR pFileName,
		LPCVOID /*pParentData*/,
		LPCVOID* ppData,
		UINT* pBytes
	) noexcept override {
		std::filesystem::path relativePath = _localDir / StrHelper::UTF8ToUTF16(pFileName);

		std::string file;
		if (!Win32Helper::ReadTextFile(relativePath.c_str(), file)) {
			return E_FAIL;
		}

		char* result = std::make_unique<char[]>(file.size()).release();
		std::memcpy(result, file.data(), file.size());

		*ppData = result;
		*pBytes = (UINT)file.size();

		return S_OK;
	}

	HRESULT CALLBACK Close(LPCVOID pData) noexcept override {
		std::unique_ptr<char[]>((char*)pData);
		return S_OK;
	}

private:
	std::filesystem::path _localDir;
};

winrt::fire_and_forget EffectsService::_CompileShaderEffectAsync(
	std::string effectName,
	std::string source,
	const phmap::flat_hash_map<std::string, float>* inlineParams,
	D3D_SHADER_MODEL shaderModel,
	std::string cacheKey,
	bool isFP16Supported,
	bool isAdvancedColorSupported,
	bool saveSources,
	bool warningsAreErrors
) noexcept {
	co_await winrt::resume_background();

	// 如果以后 _effectsMap 会变，这里应加锁
	auto it = _effectsMap.find(effectName);
	if (it == _effectsMap.end()) {
		auto lk = _shaderEffectCacheLock.lock_exclusive();
		_shaderEffectCache.erase(cacheKey);
		co_return;
	}
	const EffectInfo& effectInfo = _effects[it->second];

	ShaderEffectParserOptions options = {
		.inlineParams = inlineParams,
		.shaderModel = shaderModel
	};
	if (isFP16Supported && bool(effectInfo.flags & EffectFlags::SupportFP16)) {
		options.flags |= ShaderEffectParserFlags::EnableFP16;
	}
	if (isAdvancedColorSupported && bool(effectInfo.flags & EffectFlags::SupportAdvancedColor)) {
		options.flags |= ShaderEffectParserFlags::EnableAdvancedColor;
	}

	ShaderEffectDrawInfo effectDrawInfo;
	SmallVector<ShaderEffectSource, 0> effectSources;
	std::string errorMsg = ShaderEffectParser::ParseForDesc(
		effectInfo,
		std::move(source),
		options,
		effectDrawInfo,
		effectSources
	);
	if (!errorMsg.empty()) {
		// 解析失败
		auto lk = _shaderEffectCacheLock.lock_exclusive();
		_shaderEffectCache.erase(cacheKey);
	}

	std::wstring sourcesPath;
	if (saveSources) {
		sourcesPath = StrHelper::Concat(
			CommonSharedConstants::SOURCES_DIR, L"\\", StrHelper::UTF8ToUTF16(effectInfo.name));

		std::wstring sourcesDir = sourcesPath.substr(0, sourcesPath.find_last_of(L'\\'));
		if (!Win32Helper::DirExists(sourcesDir.c_str())) {
			Win32Helper::CreateDir(sourcesDir, true);
		}
	}

	Win32Helper::RunParallel([&](uint32_t id) {
		const auto& [source, macros] = effectSources[id];

		if (saveSources) {
			std::wstring fileName = effectDrawInfo.passes.size() == 1
				? StrHelper::Concat(sourcesPath, L".hlsl")
				: fmt::format(L"{}_Pass{}.hlsl", sourcesPath, id + 1);

			if (!Win32Helper::WriteTextFile(fileName.c_str(), source)) {
				Logger::Get().Error(fmt::format("保存 Pass{} 源码失败", id + 1));
			}
		}

		{
			winrt::com_ptr<ID3DBlob> errorMsg;

			UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_ALL_RESOURCES_BOUND;
			if (warningsAreErrors) {
				flags |= D3DCOMPILE_WARNINGS_ARE_ERRORS;
			}

#ifdef _DEBUG
			flags |= D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_DEBUG;
#else
			flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

			auto shaderMacros = std::make_unique<D3D_SHADER_MACRO[]>(macros.size() + 1);
			for (size_t i = 0; i < macros.size(); ++i) {
				shaderMacros[i] = { macros[i].first.c_str(), macros[i].second.c_str() };
			}

			std::filesystem::path includeDir = CommonSharedConstants::EFFECTS_DIR;
			size_t delimPos = effectName.find_last_of('\\');
			if (delimPos != std::string::npos) {
				includeDir /= StrHelper::UTF8ToUTF16(std::string_view(effectName.c_str(), delimPos));
			}
			FXCInclude fxcInclude(std::move(includeDir));

			HRESULT hr = D3DCompile(
				source.data(),
				source.size(),
				fmt::format("{}_Pass{}.hlsl", effectName, id + 1).c_str(),
				shaderMacros.get(),
				&fxcInclude,
				"__M",
				"cs_5_1",
				flags,
				0,
				effectDrawInfo.passes[id].byteCode.put(),
				errorMsg.put()
			);
			if (FAILED(hr)) {
				if (errorMsg) {
					Logger::Get().ComError(StrHelper::Concat("编译计算着色器失败: ", (const char*)errorMsg->GetBufferPointer()), hr);
				}
				return;
			}

			// 警告消息
			if (errorMsg) {
				Logger::Get().Warn(StrHelper::Concat("编译计算着色器时产生警告: ", (const char*)errorMsg->GetBufferPointer()));
			}
		}
	}, (uint32_t)effectDrawInfo.passes.size());

	for (const ShaderEffectPassDesc& passDesc : effectDrawInfo.passes) {
		if (!passDesc.byteCode) {
			// 编译失败
			auto lk = _shaderEffectCacheLock.lock_exclusive();
			_shaderEffectCache.erase(cacheKey);
			co_return;
		}
	}

	auto lk = _shaderEffectCacheLock.lock_exclusive();

	auto it1 = _shaderEffectCache.find(cacheKey);
	if (it1 != _shaderEffectCache.end()) {
		it1->second.drawInfo = std::move(effectDrawInfo);
	}
}

}

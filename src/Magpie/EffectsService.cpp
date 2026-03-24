#include "pch.h"
#include "CommonSharedConstants.h"
#include "ShaderEffectParser.h"
#include "EffectsService.h"
#include "Logger.h"
#include "StrHelper.h"
#include "Win32Helper.h"

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
		EffectInfo2 effectInfo;
		std::string errorMsg = ShaderEffectParser::ParseForInfo(
			StrHelper::UTF16ToUTF8(effectNames[id]), std::move(source), effectInfo);
		if (!errorMsg.empty()) {
			return;
		}
		
		auto lock = srwLock.lock_exclusive();
		_effectsMap.emplace(effectNames[id], (uint32_t)_effects.size());
		_effects.emplace_back(std::move(effectInfo));
	}, nEffect);

	_initialized.store(true, std::memory_order_release);
	_initialized.notify_one();
}

void EffectsService::Uninitialize() {
	// 等待解析完成，防止退出时崩溃
	_WaitForInitialize();
}

const std::vector<EffectInfo2>& EffectsService::GetEffects() noexcept {
	_WaitForInitialize();
	return _effects;
}

const EffectInfo2* EffectsService::GetEffect(std::wstring_view name) noexcept {
	_WaitForInitialize();

	auto it = _effectsMap.find(name);
	return it != _effectsMap.end() ? &_effects[it->second] : nullptr;
}

void EffectsService::_WaitForInitialize() noexcept {
	if (_initializedCache) {
		return;
	}

	_initialized.wait(false, std::memory_order_acquire);
	_initializedCache = true;
}

}

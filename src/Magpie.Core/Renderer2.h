#pragma once
#include "EffectDesc.h"
#include "ScalingOptions.h"

namespace Magpie {

class Renderer2 {
public:
	Renderer2() noexcept;
	~Renderer2() noexcept;

	Renderer2(const Renderer2&) = delete;
	Renderer2(Renderer2&&) = delete;

	ScalingError Initialize(HWND hwndAttach, OverlayOptions& overlayOptions) noexcept;

	bool Render(bool force = false, bool waitForGpu = false) noexcept;

	bool OnResize() noexcept;

	void OnEndResize() noexcept;

	void OnMove() noexcept;

	void SwitchToolbarState() noexcept;

	const RECT& SrcRect() const noexcept;

	// 屏幕坐标而不是窗口局部坐标
	const RECT& DestRect() const noexcept {
		return _destRect;
	}

	void OnCursorVisibilityChanged(bool isVisible, bool onDestory);

	void MessageHandler(UINT msg, WPARAM wParam, LPARAM lParam) noexcept;

	const std::vector<const EffectDesc*>& ActiveEffectDescs() const noexcept {
		return _activeEffectDescs;
	}

	void StartProfile() noexcept;

	void StopProfile() noexcept;

	bool IsCursorOnOverlayCaptionArea() const noexcept {
		return false;
	}

	winrt::fire_and_forget TakeScreenshot(
		uint32_t effectIdx,
		uint32_t passIdx = std::numeric_limits<uint32_t>::max(),
		uint32_t outputIdx = std::numeric_limits<uint32_t>::max()
	) noexcept;

private:
	RECT _destRect{};
	std::vector<const EffectDesc*> _activeEffectDescs;
};

}

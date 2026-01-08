#pragma once
#include "CursorDrawer2.h"
#include "FrameProducer.h"
#include "GraphicsContext.h"
#include "ScalingOptions.h"

namespace Magpie {

class SwapChainPresenter;

class Renderer2 {
public:
	Renderer2() noexcept;
	~Renderer2() noexcept;

	Renderer2(const Renderer2&) = delete;
	Renderer2(Renderer2&&) = delete;

	ScalingError Initialize(
		HWND hwndAttach,
		HMONITOR hMonitor,
		Size size,
		const RECT& srcRect,
		OverlayOptions& overlayOptions
	) noexcept;

	ComponentState Render(bool waitForGpu = false, bool* waitingForFirstFrame = nullptr) noexcept;

	const Rect& GetOutputRect() const noexcept {
		return _outputRect;
	}

	void OnMonitorChanged(HMONITOR hMonitor) noexcept;

	void OnResizeStarted() noexcept;

	void OnResizeEnded() noexcept;

	void OnResized(Size size) noexcept;

	void OnMsgDisplayChanged() noexcept;

	void OnCursorVisibilityChanged(bool isVisible, bool onDestory) noexcept;

private:
	void _TryInitDisplayInfo() noexcept;

	bool _UpdateColorInfo() noexcept;

	HRESULT _UpdateColorSpace() noexcept;

	HRESULT _RenderImpl(bool waitForGpu = false) noexcept;

	void _UpdateOutputRect(Size outputSize) noexcept;

	bool _CheckResult(bool success, std::string_view errorMsg) noexcept;

	bool _CheckResult(HRESULT hr, std::string_view errorMsg) noexcept;

	// 不使用 Initializing 状态
	ComponentState _state = ComponentState::NoError;

	winrt::DisplayInformation _displayInfo{ nullptr };
	winrt::DisplayInformation::AdvancedColorInfoChanged_revoker _acInfoChangedRevoker;

	Rect _outputRect{};

	GraphicsContext _graphicsContext;
	FrameProducer _frameProducer;
	CursorDrawer2 _cursorDrawer;
	std::unique_ptr<SwapChainPresenter> _presenter;
	
	HMONITOR _hCurMonitor = NULL;
	ColorInfo _colorInfo;

	uint64_t _lastProducerFrameNumber = 0;
};

}

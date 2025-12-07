#pragma once
#include "ColorInfo.h"
#include "GraphicsContext.h"
#include "ScalingOptions.h"
#include "SharedRingBuffer.h"

namespace Magpie {

class SwapChainPresenter;
class FrameProducer;

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

	ComponentState Render(bool force = false, bool waitForGpu = false) noexcept;

	void OnMonitorChanged(HMONITOR hMonitor) noexcept;

	void OnSizeChanged(Size size) noexcept;

	void OnResizeStarted() noexcept;

	void OnResizeEnded() noexcept;

	void OnMsgDisplayChanged() noexcept;

private:
	void _TryInitDisplayInfo() noexcept;

	bool _UpdateColorInfo() noexcept;

	HRESULT _UpdateColorSpace() noexcept;

	bool _CheckResult(bool success, std::string_view errorMsg) noexcept;

	bool _CheckResult(HRESULT hr, std::string_view errorMsg) noexcept;

	// 不使用 Initializing 状态
	ComponentState _state = ComponentState::NoError;

	winrt::DisplayInformation _displayInfo{ nullptr };
	winrt::DisplayInformation::AdvancedColorInfoChanged_revoker _acInfoChangedRevoker;

	RECT _outputRect{};

	GraphicsContext _graphicsContext;
	SharedRingBuffer _sharedRingBuffer;
	std::unique_ptr<FrameProducer> _frameProducer;
	std::unique_ptr<SwapChainPresenter> _presenter;
	
	HMONITOR _hCurMonitor = NULL;
	ColorInfo _colorInfo;
};

}

#pragma once
#include "EffectDesc.h"
#include "ScalingOptions.h"

namespace Magpie {

class SwapChainPresenter;

class Renderer2 {
public:
	Renderer2() noexcept;
	~Renderer2() noexcept;

	Renderer2(const Renderer2&) = delete;
	Renderer2(Renderer2&&) = delete;

	ScalingError Initialize(HWND hwndAttach, OverlayOptions& overlayOptions) noexcept;

	bool Render(bool force = false, bool waitForGpu = false, bool onHandlingDeviceLost = false) noexcept;

	bool OnSizeChanged() noexcept;

	bool OnResizeEnded() noexcept;

	void OnMove() noexcept;

	bool OnSrcMonitorChanged() noexcept;

	bool OnDisplayChanged() noexcept;

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
	HRESULT _CreateDXGIFactory() noexcept;

	bool _CreateAdapterAndDevice(GraphicsCardId graphicsCardId) noexcept;

	bool _TryCreateD3DDevice(const winrt::com_ptr<IDXGIAdapter1>& adapter, const DXGI_ADAPTER_DESC1& adapterDesc) noexcept;

	void _TryInitDisplayInfo() noexcept;

	HRESULT _UpdateAdvancedColorInfo() noexcept;

	HRESULT _UpdateAdvancedColor(bool onInit = false, bool noRender = false) noexcept;

	void _ProducerThreadProc() noexcept;

	bool _InitProducer() noexcept;

	HRESULT _CheckDeviceLost(HRESULT hr, bool onHandlingDeviceLost = false) noexcept;

	std::thread _producerThread;
	winrt::DispatcherQueue _producerThreadDispatcher{ nullptr };

	winrt::DisplayInformation _displayInfo{ nullptr };
	winrt::DisplayInformation::AdvancedColorInfoChanged_revoker _acInfoChangedRevoker;

	RECT _destRect{};

	winrt::com_ptr<IDXGIFactory7> _dxgiFactory;
	winrt::com_ptr<IDXGIAdapter4> _dxgiAdapter;
	winrt::com_ptr<ID3D12Device5> _device;

	std::unique_ptr<SwapChainPresenter> _presenter;

	winrt::com_ptr<ID3D12CommandQueue> _consumerCommandQueue;
	winrt::com_ptr<ID3D12GraphicsCommandList> _consumerCommandList;
	std::vector<winrt::com_ptr<ID3D12CommandAllocator>> _consumerCommandAllocators;

	winrt::com_ptr<ID3D12CommandQueue> _producerCommandQueue;
	winrt::com_ptr<ID3D12GraphicsCommandList> _producerCommandList;
	std::vector<winrt::com_ptr<ID3D12CommandAllocator>> _producerCommandAllocators;

	wil::srwlock _frameBufferLock;

	std::vector<winrt::com_ptr<ID3D12Resource>> _frameBuffers;
	std::vector<uint64_t> _frameBufferFenceValues;
	uint32_t _curBufferIndex = 0;

	winrt::com_ptr<ID3D12Fence1> _frameBufferFence;
	uint64_t _curFrameBufferFenceValue = 0;
	wil::unique_event_nothrow _fenceEvent;

	std::vector<const EffectDesc*> _activeEffectDescs;

	winrt::AdvancedColorKind _curAcKind = winrt::AdvancedColorKind::StandardDynamicRange;
	// HDR 模式下最大亮度缩放
	float _maxLuminance = 1.0f;
	// HDR 模式下 SDR 内容亮度缩放
	float _sdrWhiteLevel = 1.0f;

	bool _isFP16Supported = false;
	bool _isUsingWarp = false;
};

}

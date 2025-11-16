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

	void _BackendThreadProc() noexcept;

	HRESULT _CheckDeviceLost(HRESULT hr, bool onHandlingDeviceLost = false) noexcept;

	RECT _destRect{};

	winrt::com_ptr<IDXGIFactory7> _dxgiFactory;
	winrt::com_ptr<IDXGIAdapter4> _dxgiAdapter;
	winrt::com_ptr<ID3D12Device5> _device;

	winrt::com_ptr<ID3D12CommandQueue> _consumerCommandQueue;
	winrt::com_ptr<ID3D12GraphicsCommandList> _consumerCommandList;
	std::vector<winrt::com_ptr<ID3D12CommandAllocator>> _consumerCommandAllocators;

	std::unique_ptr<SwapChainPresenter> _presenter;

	std::thread _backendThread;

	std::vector<const EffectDesc*> _activeEffectDescs;

	bool _isFP16Supported = false;
	bool _isUsingWarp = false;
};

}

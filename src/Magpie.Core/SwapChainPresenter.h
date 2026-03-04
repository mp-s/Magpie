#pragma once

namespace Magpie {

class GraphicsContext;

class SwapChainPresenter {
public:
	SwapChainPresenter() = default;
	SwapChainPresenter(const SwapChainPresenter&) = delete;
	SwapChainPresenter(SwapChainPresenter&&) = delete;

	bool Initialize(
		GraphicsContext& graphicContext,
		HWND hwndAttach,
		Size size,
		const ColorInfo& colorInfo
	) noexcept;

	// SDR 下 rawRtvHandle 不做伽马校正，用于渲染光标
	void BeginFrame(
		ID3D12Resource** frameTex,
		D3D12_CPU_DESCRIPTOR_HANDLE& rtvHandle,
		D3D12_CPU_DESCRIPTOR_HANDLE& rawRtvHandle
	) noexcept;

	HRESULT EndFrame(bool waitForGpu = false) noexcept;

	Size GetSize() const noexcept { return _size; }

	HRESULT OnResized(Size size) noexcept;

	void OnResizeStarted() noexcept;

	HRESULT OnResizeEnded() noexcept;

	HRESULT OnColorInfoChanged(const ColorInfo& colorInfo) noexcept;

private:
	HRESULT _CreateRtvHeap() noexcept;

	HRESULT _RecreateBuffers() noexcept;

	HRESULT _CreateDisplayDependentResources() noexcept;

	GraphicsContext* _graphicContext = nullptr;

	winrt::com_ptr<IDXGISwapChain4> _dxgiSwapChain;
	wil::unique_event_nothrow _frameLatencyWaitableObject;
	std::vector<winrt::com_ptr<ID3D12Resource>> _frameBuffers;

	winrt::com_ptr<ID3D12DescriptorHeap> _rtvHeap;
	uint32_t _rtvDescriptorSize = 0;

	Size _size{};
	uint32_t _bufferCount = 0;
	bool _isScRGB = false;

	bool _isTearingSupported = false;
	bool _isRecreated = true;
	bool _isResizing = false;
};

}

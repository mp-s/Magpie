#pragma once

namespace Magpie {

class GraphicsContext;

class SwapChainPresenter {
public:
	SwapChainPresenter() = default;
	SwapChainPresenter(const SwapChainPresenter&) = delete;
	SwapChainPresenter(SwapChainPresenter&&) = delete;

	~SwapChainPresenter() noexcept;

	bool Initialize(
		GraphicsContext& graphicContext,
		HWND hwndAttach,
		Size size,
		const ColorInfo& colorInfo
	) noexcept;

	// SDR 下 rawRtvHandle 不做伽马校正，用于渲染光标
	void BeginFrame(
		ID3D12Resource** backBuffer,
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
	HRESULT _RecreateBuffers() noexcept;

	HRESULT _CreateDisplayDependentResources() noexcept;

	GraphicsContext* _graphicContext = nullptr;

	winrt::com_ptr<IDXGISwapChain4> _dxgiSwapChain;
	wil::unique_event_nothrow _frameLatencyWaitableObject;
	std::vector<winrt::com_ptr<ID3D12Resource>> _frameBuffers;

	Size _size{};
	uint32_t _bufferCount = 0;
	uint32_t _rtvBaseOffset = std::numeric_limits<uint32_t>::max();
	uint32_t _rawRtvBaseOffset = std::numeric_limits<uint32_t>::max();
	
	bool _isScRGB = false;
	bool _isTearingSupported = false;
	bool _isRecreated = true;
	bool _isResizing = false;
};

}

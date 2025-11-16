#pragma once
#include "PresenterBase.h"

namespace Magpie {

class SwapChainPresenter {
public:
	SwapChainPresenter() = default;
	SwapChainPresenter(const SwapChainPresenter&) = delete;
	SwapChainPresenter(SwapChainPresenter&&) = delete;

	~SwapChainPresenter();

	bool Initialize(
		ID3D12Device5* device,
		ID3D12CommandQueue* commandQueue,
		IDXGIFactory7* dxgiFactory,
		HWND hwndAttach,
		bool useScRGB
	) noexcept;

	uint32_t GetBufferCount() const noexcept;

	HRESULT BeginFrame(
		ID3D12Resource** frameTex,
		CD3DX12_CPU_DESCRIPTOR_HANDLE& rtvHandle,
		uint32_t& bufferIndex
	) noexcept;

	HRESULT EndFrame() noexcept;

	HRESULT RecreateBuffers(bool useScRGB) noexcept;

	HRESULT OnResizeEnded() noexcept;

private:
	HRESULT _LoadBufferResources(uint32_t bufferCount, bool useScRGB) noexcept;

	HRESULT _WaitForGpu() noexcept;

	ID3D12Device* _device = nullptr;
	ID3D12CommandQueue* _commandQueue = nullptr;

	winrt::com_ptr<IDXGISwapChain4> _dxgiSwapChain;
	wil::unique_event_nothrow _frameLatencyWaitableObject;
	std::vector<winrt::com_ptr<ID3D12Resource>> _renderTargets;
	std::vector<UINT64> _bufferFenceValues;
	UINT _bufferIndex = 0;

	winrt::com_ptr<ID3D12DescriptorHeap> _rtvHeap;
	UINT _rtvDescriptorSize = 0;

	winrt::com_ptr<ID3D12Fence1> _fence;
	UINT64 _curFenceValue = 0;
	wil::unique_event_nothrow _fenceEvent;

	bool _isTearingSupported = false;
	bool _isRecreated = true;
};

}

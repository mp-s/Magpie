#pragma once
#include "ColorInfo.h"

namespace Magpie {

class GraphicsContext;

class FrameRingBuffer {
public:
	FrameRingBuffer() = default;
	FrameRingBuffer(const FrameRingBuffer&) = delete;
	FrameRingBuffer(FrameRingBuffer&&) = delete;

	bool Initialize(ID3D12Device5* device, Size size, const ColorInfo& colorInfo) noexcept;

	ID3D12Resource* GetBuffer(uint32_t index) noexcept;

	HRESULT ProducerBeginFrame(
		ID3D12Resource*& buffer,
		D3D12_RESOURCE_STATES& state,
		ID3D12CommandQueue* commandQueue,
		D3D12_RESOURCE_STATES newState
	) noexcept;

	HRESULT ProducerEndFrame(ID3D12CommandQueue* commandQueue) noexcept;

	bool ConsumerBeginFrame(
		ID3D12Resource*& buffer,
		D3D12_RESOURCE_STATES& state,
		ID3D12Fence1*& fenceToSignal,
		UINT64& fenceValueToSignal,
		D3D12_RESOURCE_STATES newState
	) noexcept;

	HRESULT OnResized(Size size) noexcept;

	HRESULT OnColorInfoChanged(const ColorInfo& colorInfo) noexcept;

private:
	HRESULT _LoadBufferResources() noexcept;

	ID3D12Device5* _device = nullptr;

	wil::srwlock _lock;

	struct _FrameResourceSlot {
		winrt::com_ptr<ID3D12Resource> resource;
		uint64_t consumerFenceValue = 0;
		uint64_t producerFenceValue = 1;
		D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
	};

	std::vector<_FrameResourceSlot> _slots;
	uint32_t _curConsumerIdx = 0;
	uint32_t _curProducerIdx = 0;

	winrt::com_ptr<ID3D12Fence1> _consumerFence;
	uint64_t _curConsumerFenceValue = 0;
	winrt::com_ptr<ID3D12Fence1> _producerFence;

	Size _size{};
	bool _isScRGB = false;
};

}

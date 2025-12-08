#include "pch.h"
#include "SharedRingBuffer.h"
#include "Logger.h"
#include "ScalingWindow.h"

namespace Magpie {

bool SharedRingBuffer::Initialize(ID3D12Device5* device, Size size, const ColorInfo& colorInfo) noexcept {
	const uint32_t slotCount = ScalingWindow::Get().Options().maxProducerInFlightFrames + 1;
	_slots.resize(slotCount);

	// 消费者应落后于生产者
	_curConsumerSlot = slotCount - 1;

	CD3DX12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE_DEFAULT);
	CD3DX12_RESOURCE_DESC texDesc = CD3DX12_RESOURCE_DESC::Tex2D(
		colorInfo.kind == winrt::AdvancedColorKind::StandardDynamicRange ?
			DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT,
		size.width,
		size.height,
		1, 1, 1, 0,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
	);

	for (uint32_t i = 0; i < slotCount; ++i) {
		_FrameResourceSlot& curSlot = _slots[i];
		curSlot.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

		HRESULT hr = device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE,
			&texDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&curSlot.resource));
		if (FAILED(hr)) {
			return false;
		}
	}

	HRESULT hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_consumerFence));
	if (FAILED(hr)) {
		return false;
	}

	hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_producerFence));
	if (FAILED(hr)) {
		return false;
	}

	return true;
}

HRESULT SharedRingBuffer::ProducerBeginFrame(
	ID3D12Resource*& buffer,
	D3D12_RESOURCE_STATES& state,
	ID3D12CommandQueue* commandQueue,
	D3D12_RESOURCE_STATES newState
) noexcept {
	auto lk = _lock.lock_exclusive();

	// 等待消费者命令队列不再使用 _curProducerSlot
	_FrameResourceSlot& curSlot = _slots[_curProducerSlot];
	if (_consumerFence->GetCompletedValue() < curSlot.consumerFenceValue) {
		HRESULT hr = commandQueue->Wait(_consumerFence.get(), curSlot.consumerFenceValue);
		if (FAILED(hr)) {
			Logger::Get().ComError("ID3D12CommandQueue::Wait 失败", hr);
			return hr;
		}
	}

	buffer = curSlot.resource.get();
	state = curSlot.state;
	curSlot.state = newState;

	return S_OK;
}

HRESULT SharedRingBuffer::ProducerEndFrame(ID3D12CommandQueue* commandQueue) noexcept {
	auto lk = _lock.lock_exclusive();

	HRESULT hr = commandQueue->Signal(_producerFence.get(), _slots[_curProducerSlot].producerFenceValue);
	if (FAILED(hr)) {
		return hr;
	}

	const uint32_t slotCount = (uint32_t)_slots.size();
	const uint32_t nextProducerSlot = (_curProducerSlot + 1) % slotCount;
	if (nextProducerSlot == _curConsumerSlot) {
		uint32_t nextConsumerSlot = (_curConsumerSlot + 1) % slotCount;

		uint64_t fenceValueToWait = _slots[nextConsumerSlot].producerFenceValue;
		if (_producerFence->GetCompletedValue() < fenceValueToWait) {
			lk.reset();
			// 等待新缓冲区可用
			hr = _producerFence->SetEventOnCompletion(fenceValueToWait, nullptr);
			if (FAILED(hr)) {
				return hr;
			}
			lk = _lock.lock_exclusive();

			if (_curConsumerSlot == nextProducerSlot) {
				_curConsumerSlot = nextConsumerSlot;
			}
		} else {
			_curConsumerSlot = nextConsumerSlot;
		}
	}

	uint64_t nextFenceValue = _slots[_curProducerSlot].producerFenceValue + 1;
	_curProducerSlot = nextProducerSlot;
	_slots[nextProducerSlot].producerFenceValue = nextFenceValue;

	return S_OK;
}

bool SharedRingBuffer::ConsumerBeginFrame(
	ID3D12Resource*& buffer,
	D3D12_RESOURCE_STATES& state,
	ID3D12Fence1*& fenceToSignal,
	UINT64& fenceValueToSignal,
	D3D12_RESOURCE_STATES newState
) noexcept {
	auto lk = _lock.lock_exclusive();

	if (_curConsumerSlot != _curProducerSlot) {
		const uint64_t completedFenceValue = _producerFence->GetCompletedValue();
		if (completedFenceValue == 0) {
			// 第一帧尚未完成
			return false;
		}

		const uint32_t slotCount = (uint32_t)_slots.size();
		uint32_t nextConsumerSlot = (_curConsumerSlot + 1) % slotCount;
		if (completedFenceValue >= _slots[nextConsumerSlot].producerFenceValue) {
			_curConsumerSlot = nextConsumerSlot;

			// 寻找最新帧
			while (true) {
				nextConsumerSlot = (_curConsumerSlot + 1) % slotCount;
				if (completedFenceValue < _slots[nextConsumerSlot].producerFenceValue) {
					break;
				}

				_curConsumerSlot = nextConsumerSlot;

				// 窗口很小，但有发生的可能
				if (_curConsumerSlot == _curProducerSlot) {
					break;
				}
			}
		}
	}

	fenceToSignal = _consumerFence.get();

	_FrameResourceSlot& curSlot = _slots[_curConsumerSlot];
	fenceValueToSignal = ++_curConsumerFenceValue;
	curSlot.consumerFenceValue = fenceValueToSignal;

	buffer = curSlot.resource.get();
	state = curSlot.state;
	curSlot.state = newState;

	return true;
}

}

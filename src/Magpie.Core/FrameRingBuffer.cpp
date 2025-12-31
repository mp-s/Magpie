#include "pch.h"
#include "FrameRingBuffer.h"
#include "GraphicsContext.h"
#include "Logger.h"
#include "ScalingWindow.h"
#include "DebugInfo.h"

namespace Magpie {

bool FrameRingBuffer::Initialize(
	GraphicsContext& graphicsContext,
	SmallVectorImpl<winrt::com_ptr<ID3D12Resource>>& resources
) noexcept {
	_graphicsContext = &graphicsContext;

	const uint32_t slotCount = ScalingWindow::Get().Options().maxProducerInFlightFrames + 1;
	_slots.resize(slotCount);
	assert((uint32_t)resources.size() == slotCount);

	// 消费者应落后于生产者
	_curConsumerIdx = slotCount - 1;

	ID3D12Device5* device = graphicsContext.GetDevice();

	HRESULT hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_consumerFence));
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateFence 失败", hr);
		return false;
	}

	hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_producerFence));
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateFence 失败", hr);
		return false;
	}

	UpdateResources(resources);
	
	return true;
}

ID3D12Resource* FrameRingBuffer::GetBuffer(uint32_t index) noexcept {
	auto lk = _lock.lock_shared();
	return _slots[index].resource.get();
}

HRESULT FrameRingBuffer::ProducerBeginFrame(
	uint32_t& bufferIdx,
	ID3D12CommandQueue* commandQueue
) noexcept {
	auto lk = _lock.lock_exclusive();

	// 等待消费者命令队列不再使用 _curProducerSlot
	_FrameResourceSlot& curSlot = _slots[_curProducerIdx];
	if (_consumerFence->GetCompletedValue() < curSlot.consumerFenceValue) {
		HRESULT hr = commandQueue->Wait(_consumerFence.get(), curSlot.consumerFenceValue);
		if (FAILED(hr)) {
			Logger::Get().ComError("ID3D12CommandQueue::Wait 失败", hr);
			return hr;
		}
	}

	bufferIdx = _curProducerIdx;
	return S_OK;
}

HRESULT FrameRingBuffer::ProducerEndFrame(ID3D12CommandQueue* commandQueue) noexcept {
	auto lk = _lock.lock_exclusive();

	HRESULT hr = commandQueue->Signal(_producerFence.get(), _slots[_curProducerIdx].producerFenceValue);
	if (FAILED(hr)) {
		Logger::Get().ComError("ID3D12CommandQueue::Signal 失败", hr);
		return hr;
	}

	const uint32_t slotCount = (uint32_t)_slots.size();
	const uint32_t nextProducerSlot = (_curProducerIdx + 1) % slotCount;
	if (nextProducerSlot == _curConsumerIdx) {
		uint32_t nextConsumerSlot = (_curConsumerIdx + 1) % slotCount;

		uint64_t fenceValueToWait = _slots[nextConsumerSlot].producerFenceValue;
		if (_producerFence->GetCompletedValue() < fenceValueToWait) {
			lk.reset();
			// 等待新缓冲区可用
			hr = _producerFence->SetEventOnCompletion(fenceValueToWait, nullptr);
			if (FAILED(hr)) {
				Logger::Get().ComError("ID3D12Fence::SetEventOnCompletion 失败", hr);
				return hr;
			}
			lk = _lock.lock_exclusive();

			if (_curConsumerIdx == nextProducerSlot) {
				_curConsumerIdx = nextConsumerSlot;
			}
		} else {
			_curConsumerIdx = nextConsumerSlot;
		}
	}

	uint64_t nextFenceValue = _slots[_curProducerIdx].producerFenceValue + 1;
	_curProducerIdx = nextProducerSlot;
	_slots[nextProducerSlot].producerFenceValue = nextFenceValue;

#ifdef MP_DEBUG_INFO
	{
		auto debugLock = DEBUG_INFO.lock.lock_exclusive();
		// 在这里计算的 consumerLatency 不准确
		DEBUG_INFO.producerFrameNumber = (uint32_t)nextFenceValue;
	}
#endif

	return S_OK;
}

bool FrameRingBuffer::ConsumerBeginFrame(ID3D12Resource*& buffer, UINT64& fenceValueToSignal) noexcept {
	auto lk = _lock.lock_exclusive();

	if (_curConsumerIdx != _curProducerIdx) {
		const uint64_t completedFenceValue = _producerFence->GetCompletedValue();
		if (completedFenceValue == 0) {
			// 第一帧尚未完成
			return false;
		}

		const uint32_t slotCount = (uint32_t)_slots.size();
		uint32_t nextConsumerSlot = (_curConsumerIdx + 1) % slotCount;
		if (completedFenceValue >= _slots[nextConsumerSlot].producerFenceValue) {
			_curConsumerIdx = nextConsumerSlot;

			// 寻找最新帧
			while (true) {
				nextConsumerSlot = (_curConsumerIdx + 1) % slotCount;
				if (completedFenceValue < _slots[nextConsumerSlot].producerFenceValue) {
					break;
				}

				_curConsumerIdx = nextConsumerSlot;

				// 窗口很小，但有发生的可能
				if (_curConsumerIdx == _curProducerIdx) {
					break;
				}
			}
		}
	}

	_FrameResourceSlot& curSlot = _slots[_curConsumerIdx];
	fenceValueToSignal = ++_curConsumerFenceValue;
	curSlot.consumerFenceValue = fenceValueToSignal;

	buffer = curSlot.resource.get();
	return true;
}

HRESULT FrameRingBuffer::ConsumerEndFrame(
	ID3D12CommandQueue* commandQueue,
	UINT64 fenceValueToSignal
) const noexcept {
	HRESULT hr = commandQueue->Signal(_consumerFence.get(), fenceValueToSignal);
	if (FAILED(hr)) {
		Logger::Get().ComError("ID3D12CommandQueue::Signal 失败", hr);
		return hr;
	}

#ifdef MP_DEBUG_INFO
	{
		auto debugLock = DEBUG_INFO.lock.lock_exclusive();
		DEBUG_INFO.consumerFrameNumber = (uint32_t)_slots[_curConsumerIdx].producerFenceValue;
		DEBUG_INFO.consumerLatency = DEBUG_INFO.producerFrameNumber - DEBUG_INFO.consumerFrameNumber;
	}
#endif

	return S_OK;
}

HRESULT FrameRingBuffer::SetEventOnNewFrame(uint64_t& frameNumber, HANDLE hEvent) const noexcept {
	assert(frameNumber != 0);

	HRESULT hr = _producerFence->SetEventOnCompletion(frameNumber, hEvent);
	if (FAILED(hr)) {
		Logger::Get().ComError("ID3D12Fence::SetEventOnCompletion 失败", hr);
		return hr;
	}

	// 下一个要等待的值
	frameNumber = std::max(GetLatestFrameNumber(), frameNumber) + 1;
	return S_OK;
}

uint64_t FrameRingBuffer::GetLatestFrameNumber() const noexcept {
	return _producerFence->GetCompletedValue();
}

void FrameRingBuffer::UpdateResources(
	SmallVectorImpl<winrt::com_ptr<ID3D12Resource>>& resources
) noexcept {
	const size_t slotCount = _slots.size();
	for (size_t i = 0; i < slotCount; ++i) {
		_slots[i].resource = std::move(resources[i]);
	}
}

}

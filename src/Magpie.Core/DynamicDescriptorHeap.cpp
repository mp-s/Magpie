#include "pch.h"
#include "DynamicDescriptorHeap.h"
#include "Logger.h"

namespace Magpie {

static uint32_t INITIAL_CAPACITY = 1024;

DynamicDescriptorHeap::~DynamicDescriptorHeap() noexcept {
	// DEBUG 配置下退出前确保所有槽位都已释放
	assert(_freeBlocks.size() == 1 && *_freeBlocks.begin() == std::make_pair(_capacity, _capacity));
}

bool DynamicDescriptorHeap::Initialize(ID3D12Device5* device) noexcept {
	_device = device;
	_capacity = INITIAL_CAPACITY;
	_freeBlocks.emplace(_capacity, _capacity);

	_descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	if (FAILED(_CreateHeap())) {
		Logger::Get().Error("_CreateHeap 失败");
		return false;
	}

	return true;
}

static uint32_t GetBlockOffset(const std::pair<const uint32_t, uint32_t>& freeBlock) noexcept {
	return freeBlock.first - freeBlock.second;
}

HRESULT DynamicDescriptorHeap::Alloc(uint32_t count, uint32_t& idx) noexcept {
	auto lk = _lock.lock_exclusive();
	
	for (auto it = _freeBlocks.begin(); it != _freeBlocks.end(); ++it) {
		auto& [blockEnd, blockSize] = *it;

		// 寻找第一个足够大的空闲块
		if (blockSize >= count) {
			idx = blockEnd - blockSize;

			if (blockSize == count) {
				_freeBlocks.erase(it);
			} else {
				blockSize -= count;
			}

			return S_OK;
		}
	}

	// 扩容
	const uint32_t oldCapacity = _capacity;
	uint32_t newSlotCount;
	do {
		_capacity *= 2;
		newSlotCount = _capacity - oldCapacity;
	} while (newSlotCount <= count);
	
	bool canMergeLast = false;
	if (!_freeBlocks.empty()) {
		auto lastIt = std::prev(_freeBlocks.end());
		if (lastIt->first == oldCapacity) {
			canMergeLast = true;
			// 扩容最后一个空闲块并分配
			idx = GetBlockOffset(*lastIt);
			uint32_t newBlockSize = lastIt->second + newSlotCount - count;
			_freeBlocks.erase(lastIt);
			_freeBlocks.emplace(_capacity, newBlockSize);
		}
	}
	if (!canMergeLast) {
		idx = oldCapacity;
		_freeBlocks.emplace(_capacity, newSlotCount - count);
	}

	_retiredHeaps.push_back(_RetiredHeap{
		.heap = std::move(_curHeap),
		.producerCompleteFenceValue = _producerCompleteFenceValue,
		.consumerCompleteFenceValue = _consumerCompleteFenceValue
	});
	winrt::com_ptr<ID3D12DescriptorHeap> oldShaderInvisibleHeap = std::move(_curShaderInvisibleHeap);
	D3D12_CPU_DESCRIPTOR_HANDLE oldShaderInvisibleCpuHandle = _shaderInvisibleCpuHandle;

	HRESULT hr = _CreateHeap();
	if (FAILED(hr)) {
		Logger::Get().ComError("_CreateHeap 失败", hr);
		return hr;
	}

	_device->CopyDescriptorsSimple(oldCapacity, _shaderInvisibleCpuHandle,
		oldShaderInvisibleCpuHandle, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	_device->CopyDescriptorsSimple(oldCapacity, _cpuHandle,
		oldShaderInvisibleCpuHandle, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	return S_OK;
}

HRESULT DynamicDescriptorHeap::Free(uint32_t idx, uint32_t count) noexcept {
	assert(idx != std::numeric_limits<uint32_t>::max() && idx + count <= _capacity);

	auto lk = _lock.lock_exclusive();

	const auto freeBlocksEnd = _freeBlocks.end();

	// 寻找 idx 之后的第一个空闲块
	auto upperBoundIt = _freeBlocks.upper_bound(idx);
	auto prevIt = upperBoundIt == _freeBlocks.begin() ? freeBlocksEnd : std::prev(upperBoundIt);

	assert(upperBoundIt == freeBlocksEnd || idx + count <= GetBlockOffset(*upperBoundIt));
	assert(prevIt == freeBlocksEnd || idx >= prevIt->first);

	const bool canMergePrev = prevIt != freeBlocksEnd && idx == prevIt->first;
	const bool canMergeNext = upperBoundIt != freeBlocksEnd &&
		idx + count == GetBlockOffset(*upperBoundIt);

	if (canMergeNext) {
		upperBoundIt->second += count;

		if (canMergePrev) {
			upperBoundIt->second += prevIt->second;
			_freeBlocks.erase(prevIt);
		}
	} else {
		uint32_t newBlockSize = count;
		if (canMergePrev) {
			newBlockSize += prevIt->second;
			_freeBlocks.erase(prevIt);
		}
		_freeBlocks.emplace(idx + count, newBlockSize);
	}

	return S_OK;
}

void DynamicDescriptorHeap::CreateShaderResourceView(
	ID3D12Resource* pResource,
	const D3D12_SHADER_RESOURCE_VIEW_DESC* pDesc,
	uint32_t idx
) noexcept {
	auto lk = _lock.lock_shared();

	CD3DX12_CPU_DESCRIPTOR_HANDLE cpuHandle(_shaderInvisibleCpuHandle, idx, _descriptorSize);
	_device->CreateShaderResourceView(pResource, pDesc, cpuHandle);

	_device->CopyDescriptorsSimple(
		1,
		CD3DX12_CPU_DESCRIPTOR_HANDLE(_cpuHandle, idx, _descriptorSize),
		cpuHandle,
		D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV
	);
}

void DynamicDescriptorHeap::CreateShaderResourceViews(
	const SmallVectorImpl<ID3D12Resource*>& resources,
	const D3D12_SHADER_RESOURCE_VIEW_DESC* pDesc,
	uint32_t baseIdx
) noexcept {
	auto lk = _lock.lock_shared();

	CD3DX12_CPU_DESCRIPTOR_HANDLE cpuHandle(_shaderInvisibleCpuHandle, baseIdx, _descriptorSize);
	for (ID3D12Resource* resource : resources) {
		_device->CreateShaderResourceView(resource, pDesc, cpuHandle);
		cpuHandle.Offset(_descriptorSize);
	}

	_device->CopyDescriptorsSimple(
		(UINT)resources.size(),
		CD3DX12_CPU_DESCRIPTOR_HANDLE(_cpuHandle, baseIdx, _descriptorSize),
		CD3DX12_CPU_DESCRIPTOR_HANDLE(_shaderInvisibleCpuHandle, baseIdx, _descriptorSize),
		D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV
	);
}

void DynamicDescriptorHeap::CreateUnorderedAccessView(
	ID3D12Resource* pResource,
	const D3D12_UNORDERED_ACCESS_VIEW_DESC* pDesc,
	uint32_t idx
) noexcept {
	auto lk = _lock.lock_shared();

	CD3DX12_CPU_DESCRIPTOR_HANDLE cpuHandle(_shaderInvisibleCpuHandle, idx, _descriptorSize);
	_device->CreateUnorderedAccessView(pResource, nullptr, pDesc, cpuHandle);

	_device->CopyDescriptorsSimple(
		1,
		CD3DX12_CPU_DESCRIPTOR_HANDLE(_cpuHandle, idx, _descriptorSize),
		cpuHandle,
		D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV
	);
}

void DynamicDescriptorHeap::CreateUnorderedAccessViews(
	const SmallVectorImpl<ID3D12Resource*>& resources,
	const D3D12_UNORDERED_ACCESS_VIEW_DESC* pDesc,
	uint32_t baseIdx
) noexcept {
	auto lk = _lock.lock_shared();

	CD3DX12_CPU_DESCRIPTOR_HANDLE cpuHandle(_shaderInvisibleCpuHandle, baseIdx, _descriptorSize);
	for (ID3D12Resource* resource : resources) {
		_device->CreateUnorderedAccessView(resource, nullptr, pDesc, cpuHandle);
		cpuHandle.Offset(_descriptorSize);
	}

	_device->CopyDescriptorsSimple(
		(UINT)resources.size(),
		CD3DX12_CPU_DESCRIPTOR_HANDLE(_cpuHandle, baseIdx, _descriptorSize),
		CD3DX12_CPU_DESCRIPTOR_HANDLE(_shaderInvisibleCpuHandle, baseIdx, _descriptorSize),
		D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV
	);
}

ID3D12DescriptorHeap* DynamicDescriptorHeap::GetHeapForBinding(
	D3D12_GPU_DESCRIPTOR_HANDLE& gpuHandle,
	uint64_t completeFenceValue,
	uint64_t curFenceValue,
	bool isConsumer
) noexcept {
	auto lk = _lock.lock_exclusive();
	gpuHandle = _gpuHandle;

	if (isConsumer) {
		_consumerCompleteFenceValue = completeFenceValue;
		_consumerCurFenceValue = curFenceValue;
	} else {
		_producerCompleteFenceValue = completeFenceValue;
		_producerCurFenceValue = curFenceValue;
	}

	if (!_retiredHeaps.empty()) {
		auto it = std::find_if(
			_retiredHeaps.begin(),
			_retiredHeaps.end(),
			[&](const _RetiredHeap& retiredHeap) {
				return retiredHeap.producerCompleteFenceValue > _producerCurFenceValue ||
					retiredHeap.consumerCompleteFenceValue > _consumerCurFenceValue;
			}
		);
		if (it != _retiredHeaps.begin()) {
			_retiredHeaps.erase(_retiredHeaps.begin(), it);
		}
	}

	return _curHeap.get();
}

HRESULT DynamicDescriptorHeap::_CreateHeap() noexcept {
	D3D12_DESCRIPTOR_HEAP_DESC desc = {
		.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
		.NumDescriptors = _capacity
	};

	HRESULT hr = _device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&_curShaderInvisibleHeap));
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateDescriptorHeap 失败", hr);
		return hr;
	}

	desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	hr = _device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&_curHeap));
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateDescriptorHeap 失败", hr);
		return hr;
	}

	_shaderInvisibleCpuHandle = _curShaderInvisibleHeap->GetCPUDescriptorHandleForHeapStart();
	_cpuHandle = _curHeap->GetCPUDescriptorHandleForHeapStart();
	_gpuHandle = _curHeap->GetGPUDescriptorHandleForHeapStart();

	return S_OK;
}

}

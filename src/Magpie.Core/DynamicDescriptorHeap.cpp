#include "pch.h"
#include "DynamicDescriptorHeap.h"
#include "Logger.h"

namespace Magpie {

static uint32_t INITIAL_CAPACITY = 1024;

bool DynamicDescriptorHeap::Initialize(ID3D12Device5* device) noexcept {
	_device = device;
	_capacity = INITIAL_CAPACITY;
#ifdef _DEBUG
	// 调试时从索引 1 开始分配以便于发现错误
	_freeBlocks.emplace(_capacity, _capacity - 1);
#else
	_freeBlocks.emplace(_capacity, _capacity);
#endif

	_descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	if (FAILED(_CreateHeap())) {
		Logger::Get().Error("_CreateHeap 失败");
		return false;
	}

	return true;
}

HRESULT DynamicDescriptorHeap::Alloc(uint32_t count, uint32_t& offset) noexcept {
	auto lk = _lock.lock_exclusive();
	
	for (auto it = _freeBlocks.begin(); it != _freeBlocks.end(); ++it) {
		auto& [blockEnd, blockSize] = *it;

		// 寻找第一个足够大的空闲块
		if (blockSize >= count) {
			offset = blockEnd - blockSize;

			if (blockSize == count) {
				_freeBlocks.erase(it);
			} else {
				blockSize -= count;
			}

			return S_OK;
		}
	}

	return E_FAIL;
}

static uint32_t GetBlockOffset(const std::pair<const uint32_t, uint32_t>& freeBlock) noexcept {
	return freeBlock.first - freeBlock.second;
}

HRESULT DynamicDescriptorHeap::Free(uint32_t offset, uint32_t count) noexcept {
#ifdef _DEBUG
	assert(offset != 0);
#endif
	assert(offset + count <= _capacity);

	auto lk = _lock.lock_exclusive();

	const auto freeBlocksEnd = _freeBlocks.end();

	// 寻找 offset 之后的第一个空闲块
	auto upperBoundIt = _freeBlocks.upper_bound(offset);
	auto prevIt = upperBoundIt == _freeBlocks.begin() ? freeBlocksEnd : std::prev(upperBoundIt);

	assert(upperBoundIt == freeBlocksEnd || offset + count <= GetBlockOffset(*upperBoundIt));
	assert(prevIt == freeBlocksEnd || offset >= prevIt->first);

	const bool canMergePrev = prevIt != freeBlocksEnd && offset == prevIt->first;
	const bool canMergeNext = upperBoundIt != freeBlocksEnd &&
		offset + count == GetBlockOffset(*upperBoundIt);

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
		_freeBlocks.emplace(offset + count, newBlockSize);
	}

	return S_OK;
}

ID3D12DescriptorHeap* DynamicDescriptorHeap::GetHeap() noexcept {
	auto lk = _lock.lock_shared();
	return _curHeap.get();
}

CD3DX12_CPU_DESCRIPTOR_HANDLE DynamicDescriptorHeap::GetCpuHandle(uint32_t offset) noexcept {
	auto lk = _lock.lock_shared();
	return CD3DX12_CPU_DESCRIPTOR_HANDLE(_cpuHandle, offset, _descriptorSize);
}

CD3DX12_GPU_DESCRIPTOR_HANDLE DynamicDescriptorHeap::GetGpuHandle(uint32_t offset) noexcept {
	auto lk = _lock.lock_shared();
	return CD3DX12_GPU_DESCRIPTOR_HANDLE(_gpuHandle, offset, _descriptorSize);
}

HRESULT DynamicDescriptorHeap::_CreateHeap() noexcept {
	D3D12_DESCRIPTOR_HEAP_DESC desc = {
		.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
		.NumDescriptors = _capacity,
		.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
	};

	HRESULT hr = _device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&_curHeap));
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateDescriptorHeap 失败", hr);
		return hr;
	}

	_cpuHandle = _curHeap->GetCPUDescriptorHandleForHeapStart();
	_gpuHandle = _curHeap->GetGPUDescriptorHandleForHeapStart();

	return S_OK;
}

}

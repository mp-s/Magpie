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
	assert(offset != std::numeric_limits<uint32_t>::max() && offset + count <= _capacity);

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

wil::rwlock_release_shared_scope_exit DynamicDescriptorHeap::LockForCreatingDescriptor(
	D3D12_CPU_DESCRIPTOR_HANDLE& cpuHandle
) noexcept {
	auto lk = _lock.lock_shared();
	cpuHandle = _cpuHandle;
	return lk;
}

ID3D12DescriptorHeap* DynamicDescriptorHeap::GetHeapForBinding(
	D3D12_GPU_DESCRIPTOR_HANDLE& gpuHandle
) noexcept {
	auto lk = _lock.lock_shared();
	gpuHandle = _gpuHandle;
	return _curHeap.get();
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

#include "pch.h"
#include "DescriptorHeap.h"
#include "Logger.h"

namespace Magpie {

// 如果描述符大小为 32 字节，描述符堆消耗 2MiB 显存
static uint32_t HEAP_CAPACITY = 65536;

DescriptorHeap::~DescriptorHeap() noexcept {
	// DEBUG 配置下退出前确保所有槽位都已释放
	assert(_freeBlocks.size() == 1 && *_freeBlocks.begin() == std::make_pair(HEAP_CAPACITY, HEAP_CAPACITY));
}

bool DescriptorHeap::Initialize(ID3D12Device5* device) noexcept {
	_freeBlocks.emplace(HEAP_CAPACITY, HEAP_CAPACITY);

	_descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	D3D12_DESCRIPTOR_HEAP_DESC desc = {
		.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
		.NumDescriptors = HEAP_CAPACITY,
		.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
	};

	HRESULT hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&_heap));
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateDescriptorHeap 失败", hr);
		return false;
	}

	_cpuHandle = _heap->GetCPUDescriptorHandleForHeapStart();
	_gpuHandle = _heap->GetGPUDescriptorHandleForHeapStart();

	return true;
}

HRESULT DescriptorHeap::Alloc(uint32_t count, uint32_t& offset) noexcept {
	auto lk = _freeBlocksLock.lock_exclusive();

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

	Logger::Get().Error("描述符用尽");
	return E_OUTOFMEMORY;
}

static uint32_t GetBlockOffset(const std::pair<const uint32_t, uint32_t>& freeBlock) noexcept {
	return freeBlock.first - freeBlock.second;
}

void DescriptorHeap::Free(uint32_t offset, uint32_t count) noexcept {
	assert(offset != std::numeric_limits<uint32_t>::max() && offset + count <= HEAP_CAPACITY);

	auto lk = _freeBlocksLock.lock_exclusive();

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
}

}

#pragma once
#include "SmallVector.h"
#include <parallel_hashmap/btree.h>

namespace Magpie {

class DynamicDescriptorHeap {
public:
	DynamicDescriptorHeap() = default;
	DynamicDescriptorHeap(const DynamicDescriptorHeap&) = delete;
	DynamicDescriptorHeap(DynamicDescriptorHeap&&) = delete;

	bool Initialize(ID3D12Device5* device) noexcept;

	HRESULT Alloc(uint32_t count, uint32_t& offset) noexcept;

	HRESULT Free(uint32_t offset, uint32_t count) noexcept;

	uint32_t GetDescriptorSize() const noexcept {
		// 初始化后不会改变，因此无需同步
		return _descriptorSize;
	}

	ID3D12DescriptorHeap* GetHeap() noexcept;

	CD3DX12_CPU_DESCRIPTOR_HANDLE GetCpuHandle(uint32_t offset) noexcept;

	CD3DX12_GPU_DESCRIPTOR_HANDLE GetGpuHandle(uint32_t offset) noexcept;

private:
	HRESULT _CreateHeap() noexcept;

	ID3D12Device5* _device = nullptr;
	
	// 用于同步对下面所有成员的访问
	wil::srwlock _lock;

	uint32_t _descriptorSize = 0;
	uint32_t _capacity = 0;

	winrt::com_ptr<ID3D12DescriptorHeap> _curHeap;
	CD3DX12_CPU_DESCRIPTOR_HANDLE _cpuHandle{};
	CD3DX12_GPU_DESCRIPTOR_HANDLE _gpuHandle{};
	
	struct _RetiredHeap {
		winrt::com_ptr<ID3D12DescriptorHeap> heap;
		uint64_t producerFenceValue;
		uint64_t consumerFenceValue;
	};
	SmallVector<_RetiredHeap, 1> _retiredHeaps;

	// offset+size(end) -> size
	// 以 offset+size 作为键可以大大降低删除和插入键的频率
#ifdef _DEBUG
	// phmap::btree_map 没有 natvis，调试不方便
	std::map<uint32_t, uint32_t> _freeBlocks;
#else
	phmap::btree_map<uint32_t, uint32_t> _freeBlocks;
#endif
};

}

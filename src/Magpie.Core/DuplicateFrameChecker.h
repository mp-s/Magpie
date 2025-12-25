#pragma once
#include "SmallVector.h"

namespace Magpie {

class DuplicateFrameChecker {
public:
	DuplicateFrameChecker() = default;
	DuplicateFrameChecker(const DuplicateFrameChecker&) = delete;
	DuplicateFrameChecker(DuplicateFrameChecker&&) = delete;

	~DuplicateFrameChecker() = default;

	bool Initialize(ID3D12Device5* device, const ColorInfo& colorInfo, Size frameSize) noexcept;

	HRESULT CheckFrame(ID3D12Resource* frameResource, SmallVectorImpl<Rect>& dirtyRects) noexcept;

	void OnFrameAdopted() noexcept;

	void OnCaptureStopped() noexcept;

private:
	ID3D12Device5* _device = nullptr;

	Size _frameSize{};

	winrt::com_ptr<ID3D12CommandQueue> _commandQueue;
	winrt::com_ptr<ID3D12GraphicsCommandList> _commandList;
	winrt::com_ptr<ID3D12CommandAllocator> _commandAllocator;
	winrt::com_ptr<ID3D12Resource> _resultBuffer;
	winrt::com_ptr<ID3D12Resource> _resultReadbackBuffer;
	winrt::com_ptr<ID3D12DescriptorHeap> _descriptorHeap;
	uint32_t _descriptorSize = 0;
	winrt::com_ptr<ID3D12Fence1> _fence;
	uint64_t _curFenceValue = 0;
	winrt::com_ptr<ID3D12RootSignature> _rootSignature;
	winrt::com_ptr<ID3D12PipelineState> _pipelineState;

	uint32_t _curDescriptorOffset = 0;
	uint32_t _curTargetValue = 0;

	bool _isScRGB = false;
	bool _isFirstFrame = true;
};

}

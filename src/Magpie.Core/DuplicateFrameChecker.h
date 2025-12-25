#pragma once
#include "SmallVector.h"

namespace Magpie {

class DuplicateFrameChecker {
public:
	DuplicateFrameChecker() noexcept;
	DuplicateFrameChecker(const DuplicateFrameChecker&) = delete;
	DuplicateFrameChecker(DuplicateFrameChecker&&) = delete;

	~DuplicateFrameChecker() = default;

	bool Initialize(ID3D12Device5* device, const ColorInfo& colorInfo, Size frameSize, uint32_t frameCount) noexcept;

	HRESULT CheckFrame(ID3D12Resource* frameResource, uint32_t frameIdx, SmallVectorImpl<Rect>& dirtyRects) noexcept;

	void OnFrameAdopted(uint32_t frameIdx) noexcept;

	void OnCaptureRestarted() noexcept;

private:
	HRESULT _CheckDirtyRects(uint32_t newFrameIdx, SmallVectorImpl<Rect>& dirtyRects) noexcept;

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

	// 记录已经创建的描述符
	std::vector<bool> _descriptorTracker;
	uint32_t _oldFrameIdx = std::numeric_limits<uint32_t>::max();
	uint32_t _curTargetValue = 0;

	// 用于检查重复帧
	uint16_t _nextSkipCount;
	uint16_t _framesLeft;

	bool _isScRGB = false;
	bool _isCheckingForDuplicateFrame = true;
};

}

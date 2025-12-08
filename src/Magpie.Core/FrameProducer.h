#pragma once
#include "ColorInfo.h"
#include "GraphicsContext.h"
#include "StepTimer.h"

namespace Magpie {

class SharedRingBuffer;
class GraphicsCaptureFrameSource2;

class FrameProducer {
public:
	FrameProducer() = default;
	FrameProducer(const FrameProducer&) = delete;
	FrameProducer(FrameProducer&&) = delete;

	~FrameProducer() noexcept;

	void InitializeAsync(
		ID3D12Device5* device,
		SharedRingBuffer& sharedRingBuffer,
		const RECT& srcRect,
		HMONITOR hMonSrc,
		const ColorInfo& colorInfo
	) noexcept;

	bool WaitForInitialize(Size& outputSize) noexcept;

private:
	void _ThreadProc(
		ID3D12Device5* device,
		RECT srcRect,
		HMONITOR hMonSrc,
		ColorInfo colorInfo
	) noexcept;

	bool _Initialize(
		ID3D12Device5* device,
		const RECT& srcRect,
		HMONITOR hMonSrc,
		const ColorInfo& colorInfo
	) noexcept;

	HRESULT _Render() noexcept;

	std::atomic<ComponentState> _state = ComponentState::Initializing;

	std::thread _thread;
	winrt::DispatcherQueue _dispatcher{ nullptr };

	SharedRingBuffer* _sharedRingBuffer = nullptr;

	GraphicsContext _graphicsContext;
	StepTimer _stepTimer;
	winrt::com_ptr<ID3D12DescriptorHeap> _descHeap;
	uint32_t _srvUavDescriptorSize = 0;
	std::unique_ptr<GraphicsCaptureFrameSource2> _frameSource;

	Size _outputSize{};

	bool _isFP16Supported = false;
};

}

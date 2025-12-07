#pragma once
#include "ColorInfo.h"
#include "GraphicsContext.h"
#include "StepTimer.h"

namespace Magpie {

class SharedRingBuffer;

class FrameProducer {
public:
	FrameProducer() = default;
	FrameProducer(const FrameProducer&) = delete;
	FrameProducer(FrameProducer&&) = delete;

	~FrameProducer();

	void InitializeAsync(
		ID3D12Device5* device,
		SharedRingBuffer& sharedRingBuffer,
		const RECT& srcRect,
		HMONITOR hMonSrc,
		const ColorInfo& colorInfo
	) noexcept;

	bool WaitForInitialize(uint32_t& outputWidth, uint32_t& outputHeight) noexcept;

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
	std::unique_ptr<class GraphicsCaptureFrameSource2> _frameSource;

	uint32_t _outputWidth = 0;
	uint32_t _outputHeight = 0;

	bool _isFP16Supported = false;
};

}

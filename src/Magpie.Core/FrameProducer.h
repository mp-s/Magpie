#pragma once
#include "ColorInfo.h"
#include "GraphicsContext.h"
#include "FrameRingBuffer.h"
#include "StepTimer.h"

namespace Magpie {

class GraphicsCaptureFrameSource2;

class FrameProducer {
public:
	FrameProducer() = default;
	FrameProducer(const FrameProducer&) = delete;
	FrameProducer(FrameProducer&&) = delete;

	~FrameProducer() noexcept;

	void InitializeAsync(
		ID3D12Device5* device,
		const RECT& srcRect,
		HMONITOR hMonSrc,
		const ColorInfo& colorInfo
	) noexcept;

	bool WaitForInitialize(Size& outputSize) noexcept;

	bool ConsumerBeginFrame(
		ID3D12Resource*& buffer,
		ID3D12Fence1*& fenceToSignal,
		UINT64& fenceValueToSignal
	) noexcept;

	HRESULT OnResized(Size size, Size& outputSize) noexcept;

	HRESULT OnColorInfoChanged(const ColorInfo& colorInfo) noexcept;

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

	GraphicsContext _graphicsContext;
	FrameRingBuffer _frameRingBuffer;
	StepTimer _stepTimer;
	std::unique_ptr<GraphicsCaptureFrameSource2> _frameSource;

	winrt::com_ptr<ID3D12DescriptorHeap> _descHeap;
	uint32_t _srvUavDescriptorSize = 0;
	
	Size _inputSize{};
	Size _outputSize{};

	bool _isFP16Supported = false;
};

}

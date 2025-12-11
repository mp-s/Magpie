#pragma once
#include "ColorInfo.h"
#include "GraphicsContext.h"
#include "FrameRingBuffer.h"
#include "StepTimer.h"
#include "CatumullRomEffectDrawer.h"

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
		Size rendererSize,
		HMONITOR hMonSrc,
		const ColorInfo& colorInfo
	) noexcept;

	bool WaitForInitialize(Size& outputSize) const noexcept;

	uint64_t GetLatestFrameNumber() const noexcept;

	bool ConsumerBeginFrame(
		ID3D12Resource*& buffer,
		ID3D12Fence1*& fenceToSignal,
		UINT64& fenceValueToSignal
	) noexcept;

	HRESULT OnResized(Size rendererSize, Size& outputSize) noexcept;

	HRESULT OnColorInfoChanged(const ColorInfo& colorInfo) noexcept;

private:
	void _ProducerThreadProc(
		ID3D12Device5* device,
		RECT srcRect,
		Size rendererSize,
		HMONITOR hMonSrc,
		ColorInfo colorInfo
	) noexcept;

	bool _Initialize(
		ID3D12Device5* device,
		const RECT& srcRect,
		Size rendererSize,
		HMONITOR hMonSrc,
		const ColorInfo& colorInfo
	) noexcept;

	HRESULT _Render() noexcept;

	void _MonitorThreadProc() noexcept;

	std::atomic<ComponentState> _state = ComponentState::Initializing;

	std::thread _producerThread;
	winrt::DispatcherQueue _dispatcher{ nullptr };

	std::thread _monitorThread;

	GraphicsContext _graphicsContext;
	FrameRingBuffer _frameRingBuffer;
	StepTimer _stepTimer;
	std::unique_ptr<GraphicsCaptureFrameSource2> _frameSource;

	winrt::com_ptr<ID3D12DescriptorHeap> _descriptorHeap;
	uint32_t _descriptorSize = 0;

	CatumullRomEffectDrawer _catumullRomEffectDrawer;
	
	Size _inputSize{};
	Size _outputSize{};

	bool _isFP16Supported = false;
};

}

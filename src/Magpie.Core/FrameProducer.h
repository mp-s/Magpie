#pragma once
#include "GraphicsContext.h"
#include "EffectsDrawer.h"
#include "FrameRingBuffer.h"
#include "StepTimer.h"

namespace Magpie {

class GraphicsCaptureFrameSource;

class FrameProducer {
public:
	FrameProducer() = default;
	FrameProducer(const FrameProducer&) = delete;
	FrameProducer(FrameProducer&&) = delete;

	~FrameProducer() noexcept;

	void InitializeAsync(
		const GraphicsContext& graphicsContext,
		const RECT& srcRect,
		Size rendererSize,
		HMONITOR hMonSrc,
		const ColorInfo& colorInfo
	) noexcept;

	bool WaitForInitialize(Size& outputSize) const noexcept;

	ComponentState GetState() const noexcept;

	uint64_t GetLatestFrameNumber() const noexcept;

	bool ConsumerBeginFrame(ID3D12Resource*& buffer, UINT64& fenceValueToSignal) noexcept;

	HRESULT ConsumerEndFrame(
		ID3D12CommandQueue* commandQueue,
		UINT64 fenceValueToSignal
	) const noexcept;

	HRESULT OnResized(Size rendererSize, Size& outputSize) noexcept;

	HRESULT OnColorInfoChanged(const ColorInfo& colorInfo) noexcept;

	void OnCursorVisibilityChanged(bool isVisible, bool onDestory) noexcept;

private:
	void _ProducerThreadProc(
		RECT srcRect,
		Size rendererSize,
		HMONITOR hMonSrc,
		const ColorInfo& colorInfo
	) noexcept;

	bool _Initialize(
		const RECT& srcRect,
		Size rendererSize,
		HMONITOR hMonSrc,
		const ColorInfo& colorInfo
	) noexcept;

	HRESULT _Render() noexcept;

	void _MonitorThreadProc() noexcept;

	bool _CheckResult(HRESULT hr, std::string_view errorMsg) noexcept;

	std::atomic<ComponentState> _state = ComponentState::Initializing;

	std::thread _producerThread;
	winrt::DispatcherQueue _dispatcher{ nullptr };

	std::thread _monitorThread;

	GraphicsContext _graphicsContext;
	FrameRingBuffer _frameRingBuffer;
	StepTimer _stepTimer;
	std::unique_ptr<GraphicsCaptureFrameSource> _frameSource;
	EffectsDrawer _effectsDrawer;

	Size _initalOutputSize{};
};

}

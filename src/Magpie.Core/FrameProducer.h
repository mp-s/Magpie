#pragma once
#include "GraphicsContext.h"
#include "EffectsDrawer.h"
#include "FrameRingBuffer.h"
#include "StepTimer.h"
#include "SimpleTask.h"

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
		const ColorInfo& colorInfo,
		HMONITOR hMonSrc,
		const RECT& srcRect,
		Size rendererSize,
		Size& outputSize,
		SimpleTask<bool>& task
	) noexcept;

	ComponentState GetState() const noexcept;

	uint64_t GetLatestFrameNumber() const noexcept;

	bool ConsumerBeginFrame(
		ID3D12Resource*& buffer,
		uint32_t& srvIdx,
		UINT64& fenceValueToSignal
	) noexcept;

	HRESULT ConsumerEndFrame(
		ID3D12CommandQueue* commandQueue,
		UINT64 fenceValueToSignal
	) const noexcept;

	void OnResizedAsync(Size rendererSize, Size& outputSize, SimpleTask<HRESULT>& task) noexcept;

	void OnColorInfoChangedAsync(const ColorInfo& colorInfo, SimpleTask<HRESULT>& task) noexcept;

	void OnCursorVisibilityChanged(bool isVisible, bool onDestory) noexcept;

private:
	void _ProducerThreadProc(
		const ColorInfo& colorInfo,
		HMONITOR hMonSrc,
		RECT srcRect,
		Size rendererSize,
		Size& outputSize,
		SimpleTask<bool>& initializeTask
	) noexcept;

	bool _Initialize(
		const ColorInfo& colorInfo,
		HMONITOR hMonSrc,
		const RECT& srcRect,
		Size rendererSize,
		Size& outputSize
	) noexcept;

	void _CreateInputDescriptors() noexcept;

	void _CreateOutputDescriptors() noexcept;

	HRESULT _Render() noexcept;

	void _MonitorThreadProc() noexcept;

	bool _CheckResult(HRESULT hr, std::string_view errorMsg) noexcept;

	std::atomic<ComponentState> _state = ComponentState::NoError;

	std::thread _producerThread;
	winrt::DispatcherQueue _dispatcher{ nullptr };

	std::thread _monitorThread;

	GraphicsContext _graphicsContext;
	FrameRingBuffer _frameRingBuffer;
	StepTimer _stepTimer;
	std::unique_ptr<GraphicsCaptureFrameSource> _frameSource;
	EffectsDrawer _effectsDrawer;

	uint32_t _inputSrvBaseIdx = std::numeric_limits<uint32_t>::max();
	uint32_t _outputUavBaseIdx = std::numeric_limits<uint32_t>::max();
	uint32_t _outputSrvBaseIdx = std::numeric_limits<uint32_t>::max();

	bool _isScRGB = false;
};

}

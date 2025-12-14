#include "pch.h"
#include "FrameProducer.h"
#include "CommonSharedConstants.h"
#include "GraphicsCaptureFrameSource2.h"
#include "Logger.h"
#include "ScalingWindow.h"
#include "StrHelper.h"
#include <dispatcherqueue.h>

namespace Magpie {

FrameProducer::~FrameProducer() noexcept {
	if (_producerThread.joinable()) {
		const HANDLE hThread = _producerThread.native_handle();

		if (!wil::handle_wait(hThread, 0)) {
			const DWORD threadId = GetThreadId(_producerThread.native_handle());

			while (true) {
				// 持续尝试直到 _producerThread 创建了消息队列
				PostThreadMessage(threadId, WM_QUIT, 0, 0);

				if (wil::handle_wait(hThread, 1)) {
					break;
				}
			}
		}

		_producerThread.join();
	}
}

void FrameProducer::InitializeAsync(
	ID3D12Device5* device,
	const RECT& srcRect,
	Size rendererSize,
	HMONITOR hMonSrc,
	const ColorInfo& colorInfo
) noexcept {
	_producerThread = std::thread(
		&FrameProducer::_ProducerThreadProc,
		this,
		device,
		srcRect,
		rendererSize,
		hMonSrc,
		colorInfo
	);
}

bool FrameProducer::WaitForInitialize(Size& outputSize) const noexcept {
	_state.wait(ComponentState::Initializing, std::memory_order_relaxed);
	if (_state.load(std::memory_order_acquire) == ComponentState::NoError) {
		outputSize = _outputSize;
		return true;
	} else {
		return false;
	}
}

uint64_t FrameProducer::GetLatestFrameNumber() const noexcept {
	return _frameRingBuffer.GetLatestFrameNumber();
}

bool FrameProducer::ConsumerBeginFrame(ID3D12Resource*& buffer, UINT64& fenceValueToSignal) noexcept {
	return _frameRingBuffer.ConsumerBeginFrame(buffer, fenceValueToSignal);
}

HRESULT FrameProducer::ConsumerEndFrame(
	ID3D12CommandQueue* commandQueue,
	UINT64 fenceValueToSignal
) const noexcept {
	return _frameRingBuffer.ConsumerEndFrame(commandQueue, fenceValueToSignal);
}

HRESULT FrameProducer::OnResized(Size rendererSize, Size& outputSize) noexcept {
	HRESULT hr = S_OK;
	std::atomic<bool> done = false;

	_dispatcher.TryEnqueue([&] {
		[&] {
			hr = _graphicsContext.WaitForGpu();
			if (FAILED(hr)) {
				Logger::Get().ComError("GraphicsContext::WaitForGpu 失败", hr);
				return;
			}

			_outputSize = rendererSize;
			outputSize = _outputSize;

			hr = _frameRingBuffer.OnResized(_outputSize);
			if (FAILED(hr)) {
				Logger::Get().ComError("FrameRingBuffer::OnResized 失败", hr);
				return;
			}

			{
				const uint32_t maxInFlightFrameCount =
					ScalingWindow::Get().Options().maxProducerInFlightFrames;

				SmallVector<ID3D12Resource*, 3> inputs;
				inputs.resize(maxInFlightFrameCount);
				for (uint32_t i = 0; i < maxInFlightFrameCount; ++i) {
					inputs[i] = _frameSource->GetOutput(i);
				}

				SmallVector<ID3D12Resource*, 4> outputs;
				const uint32_t frameBufferCount = maxInFlightFrameCount + 1;
				outputs.resize(frameBufferCount);
				for (uint32_t i = 0; i < frameBufferCount; ++i) {
					outputs[i] = _frameRingBuffer.GetBuffer(i);
				}

				CD3DX12_CPU_DESCRIPTOR_HANDLE descriptorCpuHandle(
					_descriptorHeap->GetCPUDescriptorHandleForHeapStart());
				CD3DX12_GPU_DESCRIPTOR_HANDLE descriptorGpuHandle(
					_descriptorHeap->GetGPUDescriptorHandleForHeapStart());

				_catumullRomEffectDrawer.OnResize(
					_inputSize, outputSize, inputs, outputs, descriptorCpuHandle, descriptorGpuHandle);
			}

			hr = _Render();
			if (FAILED(hr)) {
				Logger::Get().ComError("_Render 失败", hr);
				return;
			}

			// 等待渲染完成
			hr = _graphicsContext.WaitForGpu();
			if (FAILED(hr)) {
				Logger::Get().ComError("GraphicsContext::WaitForGpu 失败", hr);
				return;
			}
		}();
		
		done.store(true, std::memory_order_release);
		done.notify_one();
	});

	done.wait(false, std::memory_order_acquire);
	return hr;
}

HRESULT FrameProducer::OnColorInfoChanged(const ColorInfo& colorInfo) noexcept {
	HRESULT hr = S_OK;
	std::atomic<bool> done = false;

	_dispatcher.TryEnqueue([&] {
		[&] {
			hr = _graphicsContext.WaitForGpu();
			if (FAILED(hr)) {
				Logger::Get().ComError("GraphicsContext::WaitForGpu 失败", hr);
				return;
			}

			hr = _frameRingBuffer.OnColorInfoChanged(colorInfo);
			if (FAILED(hr)) {
				Logger::Get().ComError("FrameRingBuffer::OnColorInfoChanged 失败", hr);
				return;
			}

			hr = _Render();
			if (FAILED(hr)) {
				Logger::Get().ComError("_Render 失败", hr);
			}
		}();
		
		done.store(true, std::memory_order_release);
		done.notify_one();
	});

	done.wait(false, std::memory_order_acquire);
	return hr;
}

void FrameProducer::_ProducerThreadProc(
	ID3D12Device5* device,
	RECT srcRect,
	Size rendererSize,
	HMONITOR hMonSrc,
	ColorInfo colorInfo
) noexcept {
#ifdef _DEBUG
	SetThreadDescription(GetCurrentThread(), L"Magpie-缩放生产者线程");
#endif

	if (_Initialize(device, srcRect, rendererSize, hMonSrc, colorInfo)) {
		_state.store(ComponentState::NoError, std::memory_order_release);
		_state.notify_one();
	} else {
		_state.store(ComponentState::Error, std::memory_order_relaxed);
		_state.notify_one();
		return;
	}

	StepTimerStatus stepTimerStatus = StepTimerStatus::WaitingForNewFrame;
	const bool waitMsgForNewFrame = _frameSource->ShouldWaitMessageForNewFrame();

	MSG msg;
	while (true) {
		bool fpsUpdated = false;
		// WaitingForFPSLimiter 状态下新帧消息可能已被处理，不要等待消息，直到状态变化
		stepTimerStatus = _stepTimer.WaitForNextFrame(
			waitMsgForNewFrame && stepTimerStatus != StepTimerStatus::WaitingForFPSLimiter,
			fpsUpdated
		);

		if (fpsUpdated) {
			// FPS 变化时要求前端重新渲染以更新叠加层
			PostMessage(ScalingWindow::Get().Handle(),
				CommonSharedConstants::WM_FRONTEND_RENDER, 0, 0);
		}

		while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
			if (msg.message == WM_QUIT) {
				break;
			}

			DispatchMessage(&msg);
		}

		if (msg.message == WM_QUIT) {
			break;
		}

		if (stepTimerStatus == StepTimerStatus::WaitingForFPSLimiter) {
			continue;
		}

		const FrameSourceState frameSourceState = _frameSource->GetState();
		if (frameSourceState == FrameSourceState::WaitingForFirstFrame ||
			(frameSourceState == FrameSourceState::Waiting &&
				stepTimerStatus != StepTimerStatus::ForceNewFrame)) {
			continue;
		}

		HRESULT hr = _Render();
		if (FAILED(hr)) {
			Logger::Get().ComError("_Render 失败", hr);

			if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
				_state.store(ComponentState::DeviceLost, std::memory_order_relaxed);
			} else {
				_state.store(ComponentState::Error, std::memory_order_relaxed);
			}

			break;
		}
	}

	_graphicsContext.WaitForGpu();
	// 必须在创建线程释放
	_frameSource.reset();

	if (_monitorThread.joinable()) {
		const HANDLE hThread = _monitorThread.native_handle();

		if (!wil::handle_wait(hThread, 0)) {
			const DWORD threadId = GetThreadId(_monitorThread.native_handle());

			while (true) {
				// 持续尝试直到创建了消息队列
				PostThreadMessage(threadId, WM_QUIT, 0, 0);

				if (wil::handle_wait(hThread, 1)) {
					break;
				}
			}
		}

		_monitorThread.join();
	}
}

bool FrameProducer::_Initialize(
	ID3D12Device5* device,
	const RECT& srcRect,
	Size rendererSize,
	HMONITOR hMonSrc,
	const ColorInfo& colorInfo
) noexcept {
	winrt::init_apartment(winrt::apartment_type::single_threaded);

	// 创建 DispatcherQueue
	{
		winrt::Windows::System::DispatcherQueueController dqc{ nullptr };
		HRESULT hr = CreateDispatcherQueueController(
			DispatcherQueueOptions{
				.dwSize = sizeof(DispatcherQueueOptions),
				.threadType = DQTYPE_THREAD_CURRENT
			},
			(PDISPATCHERQUEUECONTROLLER*)winrt::put_abi(dqc)
		);
		if (FAILED(hr)) {
			Logger::Get().ComError("CreateDispatcherQueueController 失败", hr);
			return false;
		}

		_dispatcher = dqc.DispatcherQueue();
	}

	const uint32_t maxInFlightFrameCount = ScalingWindow::Get().Options().maxProducerInFlightFrames;
	if (!_graphicsContext.Initialize(
		device,
		maxInFlightFrameCount,
		D3D12_COMMAND_QUEUE_PRIORITY_NORMAL,
		D3D12_COMMAND_LIST_TYPE_COMPUTE,
		true
	)) {
		Logger::Get().Error("初始化 GraphicsContext 失败");
		return false;
	}

	// 检查半精度浮点支持
	{
		D3D12_FEATURE_DATA_D3D12_OPTIONS featureData{};
		HRESULT hr = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &featureData, sizeof(featureData));
		if (SUCCEEDED(hr)) {
			_isFP16Supported = featureData.MinPrecisionSupport & D3D12_SHADER_MIN_PRECISION_SUPPORT_16_BIT;
			Logger::Get().Info(StrHelper::Concat("FP16 支持: ", _isFP16Supported ? "是" : "否"));
		} else {
			Logger::Get().ComError("CheckFeatureSupport 失败", hr);
		}
	}

	_descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	_frameSource = std::make_unique<GraphicsCaptureFrameSource2>();
	if (!_frameSource->Initialize(_graphicsContext, srcRect, hMonSrc, colorInfo)) {
		Logger::Get().Error("初始化 GraphicsCaptureFrameSource2 失败");
		return false;
	}

	_inputSize.width = srcRect.right - srcRect.left;
	_inputSize.height = srcRect.bottom - srcRect.top;

	_outputSize = rendererSize;

	// 初始化效果
	uint32_t descriptorTotal;
	EffectColorSpace colorSpace = colorInfo.kind == winrt::AdvancedColorKind::StandardDynamicRange ?
		EffectColorSpace::sRGB : EffectColorSpace::scRGB;
	HRESULT hr = _catumullRomEffectDrawer.Initialize(
		_graphicsContext, _descriptorSize, _inputSize, _outputSize, true,
		colorSpace, colorSpace, maxInFlightFrameCount, maxInFlightFrameCount + 1, descriptorTotal);
	if (FAILED(hr)) {
		Logger::Get().ComError("CatumullRomEffectDrawer::Initialize 失败", hr);
		return false;
	}

	if (!_frameRingBuffer.Initialize(device, _outputSize, colorInfo)) {
		Logger::Get().Error("初始化 FrameRingBuffer 失败");
		return false;
	}

	{
		D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {
			.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
			.NumDescriptors = descriptorTotal,
			.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
		};
		hr = device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&_descriptorHeap));
		if (FAILED(hr)) {
			Logger::Get().ComError("CreateDescriptorHeap 失败", hr);
			return false;
		}
	}

	{
		SmallVector<ID3D12Resource*, 3> inputs;
		inputs.resize(maxInFlightFrameCount);
		for (uint32_t i = 0; i < maxInFlightFrameCount; ++i) {
			inputs[i] = _frameSource->GetOutput(i);
		}

		SmallVector<ID3D12Resource*, 4> outputs;
		const uint32_t frameBufferCount = maxInFlightFrameCount + 1;
		outputs.resize(frameBufferCount);
		for (uint32_t i = 0; i < frameBufferCount; ++i) {
			outputs[i] = _frameRingBuffer.GetBuffer(i);
		}

		CD3DX12_CPU_DESCRIPTOR_HANDLE descriptorCpuHandle(
			_descriptorHeap->GetCPUDescriptorHandleForHeapStart());
		CD3DX12_GPU_DESCRIPTOR_HANDLE descriptorGpuHandle(
			_descriptorHeap->GetGPUDescriptorHandleForHeapStart());

		_catumullRomEffectDrawer.CreateDeviceResources(
			inputs, outputs, descriptorCpuHandle, descriptorGpuHandle);
	}

	{
		std::optional<float> maxFrameRate;
		if (!_frameSource->ShouldWaitMessageForNewFrame()) {
			// 某些捕获方式不会限制捕获帧率，因此将捕获帧率限制为屏幕刷新率
			const HMONITOR hMon = hMonSrc;

			MONITORINFOEX mi{ { sizeof(MONITORINFOEX) } };
			GetMonitorInfo(hMon, &mi);

			DEVMODE dm{ .dmSize = sizeof(DEVMODE) };
			EnumDisplaySettings(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm);

			if (dm.dmDisplayFrequency > 0) {
				Logger::Get().Info(fmt::format("屏幕刷新率: {}", dm.dmDisplayFrequency));
				maxFrameRate = float(dm.dmDisplayFrequency);
			}
		}

		const ScalingOptions& options = ScalingWindow::Get().Options();
		if (options.maxFrameRate) {
			if (!maxFrameRate || *options.maxFrameRate < *maxFrameRate) {
				maxFrameRate = options.maxFrameRate;
			}
		}

		// 测试着色器性能时最小帧率应设为无限大，但由于 /fp:fast 下无限大不可靠，因此改为使用 max()，
		// 和无限大效果相同。
		const float minFrameRate = options.IsBenchmarkMode()
			? std::numeric_limits<float>::max() : options.minFrameRate;
		_stepTimer.Initialize(minFrameRate, maxFrameRate);
	}

	_monitorThread = std::thread(&FrameProducer::_MonitorThreadProc, this);

	// 最后启动捕获以尽可能推迟显示黄色边框 (Win10) 或禁用圆角 (Win11)
	if (!_frameSource->Start()) {
		Logger::Get().Error("GraphicsCaptureFrameSource2::Start 失败");
		return false;
	}

	return true;
}

HRESULT FrameProducer::_Render() noexcept {
	_stepTimer.PrepareForRender();

	uint32_t frameIndex;
	HRESULT hr = _graphicsContext.BeginFrame(frameIndex, nullptr);
	if (FAILED(hr)) {
		Logger::Get().ComError("GraphicsContext::BeginFrame 失败", hr);
		return hr;
	}

	ID3D12CommandQueue* commandQueue = _graphicsContext.GetCommandQueue();

	uint32_t frameRingBufferIdx;
	hr = _frameRingBuffer.ProducerBeginFrame(frameRingBufferIdx, commandQueue);
	if (FAILED(hr)) {
		Logger::Get().ComError("FrameRingBuffer::ProducerBeginFrame 失败", hr);
		return hr;
	}

	uint32_t frameSourceOutputIdx;
	hr = _frameSource->Update(frameSourceOutputIdx);
	if (FAILED(hr)) {
		Logger::Get().ComError("GraphicsCaptureFrameSource2::Update 失败", hr);
		return hr;
	}

	// 处于 COMMON 状态，使用结束后也应处于此状态
	ID3D12Resource* input = _frameSource->GetOutput(frameSourceOutputIdx);
	// 处于 COPY_SOURCE 状态，使用结束后也应处于此状态
	ID3D12Resource* output = _frameRingBuffer.GetBuffer(frameRingBufferIdx);

	ID3D12GraphicsCommandList* commandList = _graphicsContext.GetCommandList();

	{
		D3D12_RESOURCE_BARRIER barriers[] = {
			CD3DX12_RESOURCE_BARRIER::Transition(
				input, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, 0),
			CD3DX12_RESOURCE_BARRIER::Transition(
				output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 0)
		};
		commandList->ResourceBarrier((UINT)std::size(barriers), barriers);
	}

	{
		ID3D12DescriptorHeap* t = _descriptorHeap.get();
		commandList->SetDescriptorHeaps(1, &t);
	}

	_catumullRomEffectDrawer.Draw(frameSourceOutputIdx, frameRingBufferIdx);

	{
		D3D12_RESOURCE_BARRIER barriers[] = {
			CD3DX12_RESOURCE_BARRIER::Transition(
				input, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON, 0),
			CD3DX12_RESOURCE_BARRIER::Transition(
				output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE, 0)
		};
		commandList->ResourceBarrier((UINT)std::size(barriers), barriers);
	}

	hr = commandList->Close();
	if (FAILED(hr)) {
		Logger::Get().ComError("ID3D12GraphicsCommandList::Close 失败", hr);
		return hr;
	}

	commandQueue->ExecuteCommandLists(1, CommandListCast(&commandList));

	hr = _frameRingBuffer.ProducerEndFrame(commandQueue);
	if (FAILED(hr)) {
		Logger::Get().ComError("FrameRingBuffer::ProducerEndFrame 失败", hr);
		return hr;
	}

	hr = _graphicsContext.EndFrame();
	if (FAILED(hr)) {
		Logger::Get().ComError("GraphicsContext::EndFrame 失败", hr);
		return hr;
	}

	return S_OK;
}

void FrameProducer::_MonitorThreadProc() noexcept {
	wil::unique_event_nothrow event;
	if (!event.try_create(wil::EventOptions::None, nullptr)) {
		Logger::Get().Win32Error("创建事件失败");
		return;
	}

	uint64_t frameNumber = 1;

	// 绑定新帧渲染完成时触发的事件
	HRESULT hr = _frameRingBuffer.SetEventOnNewFrame(frameNumber, event.get());
	if (FAILED(hr)) {
		Logger::Get().ComError("FrameRingBuffer::SetEventOnNewFrame 失败", hr);
		return;
	}

	MSG msg;
	while (true) {
		while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
			if (msg.message == WM_QUIT) {
				return;
			}

			DispatchMessage(&msg);
		}

		// 有新帧可用时返回 WAIT_OBJECT_0，有新消息时返回 WAIT_OBJECT_0 + 1
		HANDLE hEvent = event.get();
		if (MsgWaitForMultipleObjectsEx(1, &hEvent, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE) == WAIT_OBJECT_0) {
			// 通知消费者渲染
			PostMessage(ScalingWindow::Get().Handle(), CommonSharedConstants::WM_FRONTEND_RENDER, 0, 0);

			hr = _frameRingBuffer.SetEventOnNewFrame(frameNumber, event.get());
			if (FAILED(hr)) {
				Logger::Get().ComError("FrameRingBuffer::SetEventOnNewFrame 失败", hr);
				return;
			}
		}
	}
}

}

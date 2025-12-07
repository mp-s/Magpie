#include "pch.h"
#include "FrameProducer.h"
#include "CommonSharedConstants.h"
#include "GraphicsCaptureFrameSource2.h"
#include "Logger.h"
#include "ScalingWindow.h"
#include "SharedRingBuffer.h"
#include "StrHelper.h"
#include <dispatcherqueue.h>

namespace Magpie {

FrameProducer::~FrameProducer() {
	if (_thread.joinable()) {
		const HANDLE hThread = _thread.native_handle();

		if (!wil::handle_wait(hThread, 0)) {
			const DWORD threadId = GetThreadId(_thread.native_handle());

			while (true) {
				// 持续尝试直到 _producerThread 创建了消息队列
				PostThreadMessage(threadId, WM_QUIT, 0, 0);

				if (wil::handle_wait(hThread, 1)) {
					break;
				}
			}
		}

		_thread.join();
	}
}

void FrameProducer::InitializeAsync(
	ID3D12Device5* device,
	SharedRingBuffer& sharedRingBuffer,
	const RECT& srcRect,
	HMONITOR hMonSrc,
	const ColorInfo& colorInfo
) noexcept {
	_sharedRingBuffer = &sharedRingBuffer;

	_thread = std::thread(
		&FrameProducer::_ThreadProc,
		this,
		device,
		srcRect,
		hMonSrc,
		colorInfo
	);
}

bool FrameProducer::WaitForInitialize(uint32_t& outputWidth, uint32_t& outputHeight) noexcept {
	_state.wait(ComponentState::Initializing, std::memory_order_relaxed);
	if (_state.load(std::memory_order_acquire) == ComponentState::NoError) {
		outputWidth = _outputWidth;
		outputHeight = _outputHeight;
		return true;
	} else {
		return false;
	}
}

void FrameProducer::_ThreadProc(
	ID3D12Device5* device,
	RECT srcRect,
	HMONITOR hMonSrc,
	ColorInfo colorInfo
) noexcept {
#ifdef _DEBUG
	SetThreadDescription(GetCurrentThread(), L"Magpie-缩放生产者线程");
#endif

	if (_Initialize(device, srcRect, hMonSrc, colorInfo)) {
		_state.store(ComponentState::NoError, std::memory_order_release);
		_state.notify_one();
	} else {
		_state.store(ComponentState::Error, std::memory_order_relaxed);
		_state.notify_one();
		return;
	}

	StepTimerStatus stepTimerStatus = StepTimerStatus::WaitForNewFrame;
	const bool waitMsgForNewFrame =
		_frameSource->WaitType() == FrameSourceWaitType::WaitForMessage;

	MSG msg;
	while (true) {
		bool fpsUpdated = false;
		// WaitForFPSLimiter 状态下新帧消息可能已被处理，不要等待消息，直到状态变化
		stepTimerStatus = _stepTimer.WaitForNextFrame(
			waitMsgForNewFrame && stepTimerStatus != StepTimerStatus::WaitForFPSLimiter,
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

		if (stepTimerStatus != StepTimerStatus::WaitForFPSLimiter) {
			if (stepTimerStatus == StepTimerStatus::ForceNewFrame || _frameSource->IsNewFrameAvailable()) {
				HRESULT hr = _Render();
				if (FAILED(hr)) {
					if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
						_state.store(ComponentState::DeviceLost, std::memory_order_relaxed);
					} else {
						_state.store(ComponentState::Error, std::memory_order_relaxed);
					}

					break;
				}
			}
		}
	}

	// 不能在缩放线程释放
	_frameSource.reset();
}

bool FrameProducer::_Initialize(
	ID3D12Device5* device,
	const RECT& srcRect,
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
			return;
		}

		_dispatcher = dqc.DispatcherQueue();
	}

	const uint32_t maxInFlightFrameCount = ScalingWindow::Get().Options().maxProducerInFlightFrames;
	if (!_graphicsContext.Initialize(device, maxInFlightFrameCount, D3D12_COMMAND_LIST_TYPE_COMPUTE)) {
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

	// 初始化效果
	_outputWidth = uint32_t(srcRect.right - srcRect.left);
	_outputHeight = uint32_t(srcRect.right - srcRect.left);

	if (!_sharedRingBuffer->Initialize(device, _outputWidth, _outputHeight, colorInfo)) {
		Logger::Get().Error("初始化 SharedRingBuffer 失败");
		return false;
	}

	const uint32_t sharedRingBufferCount = maxInFlightFrameCount + 1;

	{
		D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {
			.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
			.NumDescriptors = sharedRingBufferCount,
			.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
		};
		HRESULT hr = device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&_descHeap));
		if (FAILED(hr)) {
			Logger::Get().ComError("CreateDescriptorHeap 失败", hr);
			return false;
		}

		_srvUavDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	}

	_frameSource = std::make_unique<GraphicsCaptureFrameSource2>();
	if (!_frameSource->Initialize(device, _graphicsContext.GetDXGIFactoryForEnumingAdapters(),
		_graphicsContext.GetDXGIAdapter(), srcRect, hMonSrc, colorInfo)) {
		Logger::Get().Error("初始化 GraphicsCaptureFrameSource2 失败");
		return false;
	}

	{
		std::optional<float> maxFrameRate;
		if (_frameSource->WaitType() == FrameSourceWaitType::NoWait) {
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

	// 最后启动捕获以尽可能推迟显示黄色边框 (Win10) 或禁用圆角 (Win11)
	if (!_frameSource->Start()) {
		Logger::Get().Error("GraphicsCaptureFrameSource2::Start 失败");
		return false;
	}

	return false;
}

HRESULT FrameProducer::_Render() noexcept {
	_stepTimer.PrepareForRender();

	uint32_t frameIndex;
	HRESULT hr = _graphicsContext.BeginFrame(frameIndex, nullptr);
	if (!FAILED(hr)) {
		Logger::Get().ComError("GraphicsContext::BeginFrame 失败", hr);
		return hr;
	}

	ID3D12CommandQueue* commandQueue = _graphicsContext.GetCommandQueue();

	ID3D12Resource* curBuffer;
	D3D12_RESOURCE_STATES bufferState;
	hr = _sharedRingBuffer->ProducerBeginFrame(
		curBuffer, bufferState, commandQueue, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	if (FAILED(hr)) {
		Logger::Get().ComError("SharedRingBuffer::ProducerBeginFrame 失败", hr);
		return hr;
	}

	ID3D12GraphicsCommandList* commandList = _graphicsContext.GetCommandList();

	if (bufferState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
		CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
			curBuffer, bufferState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 0);
		commandList->ResourceBarrier(1, &barrier);
	}

	hr = _frameSource->Update(commandList, frameIndex);
	if (FAILED(hr)) {
		Logger::Get().ComError("GraphicsCaptureFrameSource2::Update 失败", hr);
		return hr;
	}

	ID3D12Resource* input = _frameSource->GetOutput();

	{
		D3D12_RESOURCE_BARRIER barriers[] = {
			CD3DX12_RESOURCE_BARRIER::Transition(
				input, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE, 0),
			CD3DX12_RESOURCE_BARRIER::Transition(
				curBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST, 0)
		};
		commandList->ResourceBarrier((UINT)std::size(barriers), barriers);

		commandList->CopyResource(curBuffer, input);

		barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
		barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
		barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		commandList->ResourceBarrier((UINT)std::size(barriers), barriers);
	}

	HRESULT hr = commandList->Close();
	if (FAILED(hr)) {
		Logger::Get().ComError("ID3D12GraphicsCommandList::Close 失败", hr);
		return hr;
	}

	commandQueue->ExecuteCommandLists(1, CommandListCast(&commandList));

	if (FAILED(_sharedRingBuffer->ProducerEndFrame(commandQueue))) {
		Logger::Get().ComError("SharedRingBuffer::ProducerEndFrame 失败", hr);
		return hr;
	}

	if (FAILED(_graphicsContext.EndFrame())) {
		Logger::Get().ComError("GraphicsContext::EndFrame 失败", hr);
		return hr;
	}

	return S_OK;
}

}

#include "pch.h"
#include "Renderer2.h"
#include "DebugInfo.h"
#include "FrameProducer.h"
#include "Logger.h"
#include "ScalingWindow.h"
#include "SwapChainPresenter.h"
#include "GraphicsCaptureFrameSource.h"
#include <d3dkmthk.h>
#include <windows.graphics.display.interop.h>

namespace Magpie {

static constexpr float SCENE_REFERRED_SDR_WHITE_LEVEL = 80.0f;

Renderer2::Renderer2() noexcept {}

Renderer2::~Renderer2() noexcept {
	_graphicsContext.WaitForGpu();
}

static void SetGpuPriority() noexcept {
	// 不使用 REALTIME 优先级，它可能造成系统不稳定。
	// 来自 https://github.com/obsproject/obs-studio/blob/16cb051a57bb357fe866252c1360ce2c38e2deec/libobs-d3d11/d3d11-subsystem.cpp#L429
	NTSTATUS status = D3DKMTSetProcessSchedulingPriorityClass(
		GetCurrentProcess(), D3DKMT_SCHEDULINGPRIORITYCLASS_HIGH);
	if (status != STATUS_SUCCESS) {
		Logger::Get().NTError("D3DKMTSetProcessSchedulingPriorityClass 失败", status);
	}
}

ScalingError Renderer2::Initialize(
	HWND hwndAttach,
	HMONITOR hMonitor,
	Size size,
	const RECT& srcRect,
	OverlayOptions& /*overlayOptions*/
) noexcept {
	_hCurMonitor = hMonitor;

	SetGpuPriority();

	const ScalingOptions& options = ScalingWindow::Get().Options();
	if (!_graphicsContext.Initialize(
		options.graphicsCardId,
		options.Is3DGameMode() ? 2 : 6,
		D3D12_COMMAND_QUEUE_PRIORITY_HIGH,
		D3D12_COMMAND_LIST_TYPE_DIRECT
	)) {
		Logger::Get().Error("初始化 GraphicsContext 失败");
		return ScalingError::ScalingFailedGeneral;
	}

#ifdef MP_DEBUG_INFO
	{
		ID3D12Device5* device = _graphicsContext.GetDevice();

		// 禁用动态时钟频率调整
		if (DEBUG_INFO.enableStablePower) {
			HRESULT hr = device->SetStablePowerState(TRUE);
			if (FAILED(hr)) {
				Logger::Get().ComError("SetStablePowerState 失败", hr);
			}
		}

#ifdef _DEBUG
		// 模拟低速 GPU
		winrt::com_ptr<ID3D12DebugDevice1> debugDevice;
		HRESULT hr = device->QueryInterface<ID3D12DebugDevice1>(debugDevice.put());
		if (SUCCEEDED(hr)) {
			D3D12_DEBUG_DEVICE_GPU_SLOWDOWN_PERFORMANCE_FACTOR value = {
				.SlowdownFactor = DEBUG_INFO.gpuSlowDownFactor
			};
			hr = debugDevice->SetDebugParameter(
				D3D12_DEBUG_DEVICE_PARAMETER_GPU_SLOWDOWN_PERFORMANCE_FACTOR, &value, sizeof(value));
			if (FAILED(hr)) {
				Logger::Get().ComError("ID3D12DebugDevice1::SetDebugParameter 失败", hr);
			}
		} else {
			Logger::Get().ComError("获取 ID3D12DebugDevice1 失败", hr);
		}
#endif
	}
#endif

	// 失败则回落到使用传统方法获取颜色显示能力
	_TryInitDisplayInfo();

	if (!_UpdateColorInfo()) {
		Logger::Get().Error("_UpdateColorInfo 失败");
		return ScalingError::ScalingFailedGeneral;
	}

	_frameProducer = std::make_unique<FrameProducer>();
	// 也会初始化 FrameRingBuffer
	_frameProducer->InitializeAsync(_graphicsContext, srcRect, size, hMonitor, _colorInfo);

	_presenter = std::make_unique<SwapChainPresenter>();
	if (!_presenter->Initialize(_graphicsContext, hwndAttach, size, _colorInfo)) {
		Logger::Get().Error("初始化 SwapChainPresenter 失败");
		return ScalingError::ScalingFailedGeneral;
	}

	Size outputSize;
	if (!_frameProducer->WaitForInitialize(outputSize)) {
		Logger::Get().Error("FrameProducer::WaitForInitialize 失败");
		return ScalingError::ScalingFailedGeneral;
	}

	_UpdateOutputRect(outputSize);

	return ScalingError::NoError;
}

ComponentState Renderer2::Render(bool waitForGpu, bool* waitingForFirstFrame) noexcept {
	assert(!waitingForFirstFrame || !*waitingForFirstFrame);

	if (_state != ComponentState::NoError) {
		return _state;
	}

	_state = _frameProducer->GetState();
	if (_state != ComponentState::NoError) {
		return _state;
	}

	{
		uint64_t latestProducerFrameNumber = _frameProducer->GetLatestFrameNumber();
		if (latestProducerFrameNumber == _lastProducerFrameNumber) {
			if (waitingForFirstFrame && latestProducerFrameNumber == 0) {
				*waitingForFirstFrame = true;
			}

			return _state;
		} else {
			_lastProducerFrameNumber = latestProducerFrameNumber;
		}
	}

	_CheckResult(_RenderImpl(waitForGpu), "_RenderImpl 失败");
	return _state;
}

void Renderer2::OnMonitorChanged(HMONITOR hMonitor) noexcept {
	if (_state != ComponentState::NoError) {
		return;
	}

	_acInfoChangedRevoker.revoke();
	_displayInfo = nullptr;
	_hCurMonitor = hMonitor;

	_TryInitDisplayInfo();
	_CheckResult(_UpdateColorSpace(), "_UpdateColorSpace 失败");
}

void Renderer2::OnResizeStarted() noexcept {
	if (_state == ComponentState::NoError) {
		_presenter->OnResizeStarted();
	}
}

void Renderer2::OnResizeEnded() noexcept {
	if (_state == ComponentState::NoError) {
		_CheckResult(_presenter->OnResizeEnded(), "SwapChainPresenter::OnResizeEnded 失败");
	}
}

void Renderer2::OnResized(Size size) noexcept {
	if (_state != ComponentState::NoError) {
		return;
	}

	// 会等待 GPU
	if (!_CheckResult(_presenter->OnResized(size), "SwapChainPresenter::OnResized 失败")) {
		return;
	}

	Size outputSize;
	if (!_CheckResult(_frameProducer->OnResized(size, outputSize), "FrameProducer::OnResized 失败")) {
		return;
	}

	_UpdateOutputRect(outputSize);

	_CheckResult(_RenderImpl(true), "_RenderImpl 失败");
}

void Renderer2::OnMsgDisplayChanged() noexcept {
	// winrt::DisplayInformation 可用时已通过事件监听颜色配置变化
	if (_state != ComponentState::NoError || _displayInfo) {
		return;
	}

	_CheckResult(_UpdateColorSpace(), "_UpdateColorSpace 失败");
}

void Renderer2::OnCursorVisibilityChanged(bool isVisible, bool onDestory) noexcept {
	_frameProducer->OnCursorVisibilityChanged(isVisible, onDestory);
}

void Renderer2::_TryInitDisplayInfo() noexcept {
	// 从 Win11 22H2 开始支持
	winrt::com_ptr<IDisplayInformationStaticsInterop> interop =
		winrt::try_get_activation_factory<winrt::DisplayInformation, IDisplayInformationStaticsInterop>();
	if (!interop) {
		return;
	}
	
	HRESULT hr = interop->GetForMonitor(
		_hCurMonitor, winrt::guid_of<winrt::DisplayInformation>(), winrt::put_abi(_displayInfo));
	if (FAILED(hr)) {
		Logger::Get().ComError("IDisplayInformationStaticsInterop::GetForMonitor 失败", hr);
		return;
	}

	_acInfoChangedRevoker = _displayInfo.AdvancedColorInfoChanged(
		winrt::auto_revoke,
		[this](winrt::DisplayInformation const&, winrt::IInspectable const&) {
			_CheckResult(_UpdateColorSpace(), "_UpdateColorSpace 失败");
		}
	);
}

static float GetSDRWhiteLevel(std::wstring_view monitorName) noexcept {
	UINT32 pathCount = 0, modeCount = 0;
	if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) {
		return 1.0f;
	}

	std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
	std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
	if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS) {
		return 1.0f;
	}

	for (const DISPLAYCONFIG_PATH_INFO& path : paths) {
		DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName = {
			.header = {
				.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME,
				.size = sizeof(sourceName),
				.adapterId = path.sourceInfo.adapterId,
				.id = path.sourceInfo.id
			}
		};
		if (DisplayConfigGetDeviceInfo(&sourceName.header) != ERROR_SUCCESS) {
			continue;
		}

		if (monitorName == sourceName.viewGdiDeviceName) {
			DISPLAYCONFIG_SDR_WHITE_LEVEL sdr = {
				.header = {
					.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL,
					.size = sizeof(sdr),
					.adapterId = path.targetInfo.adapterId,
					.id = path.targetInfo.id
				}
			};
			if (DisplayConfigGetDeviceInfo(&sdr.header) == ERROR_SUCCESS) {
				return sdr.SDRWhiteLevel / 1000.0f;
			} else {
				return 1.0f;
			}
		}
	}

	return 1.0f;
}

bool Renderer2::_UpdateColorInfo() noexcept {
	if (_displayInfo) {
		winrt::AdvancedColorInfo acInfo = _displayInfo.GetAdvancedColorInfo();

		_colorInfo.kind = acInfo.CurrentAdvancedColorKind();
		if (_colorInfo.kind == winrt::AdvancedColorKind::HighDynamicRange) {
			_colorInfo.maxLuminance = acInfo.MaxLuminanceInNits() / SCENE_REFERRED_SDR_WHITE_LEVEL;
			_colorInfo.sdrWhiteLevel = acInfo.SdrWhiteLevelInNits() / SCENE_REFERRED_SDR_WHITE_LEVEL;
		} else {
			_colorInfo.maxLuminance = 1.0f;
			_colorInfo.sdrWhiteLevel = 1.0f;
		}

		return true;
	}

	IDXGIFactory7* dxgiFactory = _graphicsContext.GetDXGIFactoryForEnumingAdapters();
	if (!dxgiFactory) {
		return false;
	}

	winrt::com_ptr<IDXGIAdapter1> adapter;
	winrt::com_ptr<IDXGIOutput> output;
	for (UINT adapterIdx = 0;
		SUCCEEDED(dxgiFactory->EnumAdapters1(adapterIdx, adapter.put()));
		++adapterIdx
	) {
		for (UINT outputIdx = 0;
			SUCCEEDED(adapter->EnumOutputs(outputIdx, output.put()));
			++outputIdx
		) {
			DXGI_OUTPUT_DESC1 desc;
			if (SUCCEEDED(output.try_as<IDXGIOutput6>()->GetDesc1(&desc))) {
				if (desc.Monitor == _hCurMonitor) {
					// DXGI 将 WCG 视为 SDR
					if (desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) {
						_colorInfo.kind = winrt::AdvancedColorKind::HighDynamicRange;
						_colorInfo.maxLuminance = desc.MaxLuminance / SCENE_REFERRED_SDR_WHITE_LEVEL;
						_colorInfo.sdrWhiteLevel = GetSDRWhiteLevel(desc.DeviceName);
					} else {
						_colorInfo.kind = winrt::AdvancedColorKind::StandardDynamicRange;
						_colorInfo.maxLuminance = 1.0f;
						_colorInfo.sdrWhiteLevel = 1.0f;
					}

					return true;
				}
			}
		}
	}

	// 未找到视为 SDR
	_colorInfo.kind = winrt::AdvancedColorKind::StandardDynamicRange;
	_colorInfo.maxLuminance = 1.0f;
	_colorInfo.sdrWhiteLevel = 1.0f;
	return true;
}

HRESULT Renderer2::_UpdateColorSpace() noexcept {
	ColorInfo oldColorInfo = _colorInfo;
	if (!_UpdateColorInfo()) {
		return E_FAIL;
	}

	if (oldColorInfo == _colorInfo) {
		return S_OK;
	}

	// 会等待 GPU
	HRESULT hr = _presenter->OnColorInfoChanged(_colorInfo);
	if (FAILED(hr)) {
		Logger::Get().ComError("SwapChainPresenter::OnColorInfoChanged 失败", hr);
		return hr;
	}

	return S_OK;
}

HRESULT Renderer2::_RenderImpl(bool waitForGpu) noexcept {
	// 处于 COPY_SOURCE 状态，使用结束后也应处于此状态
	ID3D12Resource* curBuffer;
	UINT64 fenceValueToSignal;
	if (!_frameProducer->ConsumerBeginFrame(curBuffer, fenceValueToSignal)) {
		// 不应出现第一帧未完成的情况
		assert(false);
		return S_OK;
	}

	// SwapChain::BeginFrame 和 GraphicsContext::BeginFrame 无顺序要求，不过
	// 前者通常等待时间更久，将它放在前面可以减少等待次数。
	ID3D12Resource* frameTex;
	CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle;
	_presenter->BeginFrame(&frameTex, rtvHandle);

	uint32_t frameIndex;
	HRESULT hr = _graphicsContext.BeginFrame(frameIndex, nullptr);
	if (FAILED(hr)) {
		Logger::Get().ComError("GraphicsContext::BeginFrame 失败", hr);
		return hr;
	}

	ID3D12GraphicsCommandList* commandList = _graphicsContext.GetCommandList();

	if (const Size size = _presenter->GetSize(); _outputRect == RECT{ 0,0,(LONG)size.width,(LONG)size.height }) {
		{
			CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
				frameTex, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST, 0);
			commandList->ResourceBarrier(1, &barrier);
		}

		commandList->CopyResource(frameTex, curBuffer);
	} else {
		{
			D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
				frameTex, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET, 0);
			commandList->ResourceBarrier(1, &barrier);
		}

		// 存在黑边时应填充背景。使用交换链呈现时需要这个操作，因为我们指定了 
		// DXGI_SWAP_EFFECT_FLIP_DISCARD，同时也是为了和 RTSS 兼容。
		static constexpr FLOAT BLACK[4] = { 0.0f,0.0f,0.0f,1.0f };
		commandList->ClearRenderTargetView(rtvHandle, BLACK, 0, nullptr);

		{
			CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
				frameTex, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST, 0);
			commandList->ResourceBarrier(1, &barrier);
		}

		CD3DX12_TEXTURE_COPY_LOCATION src(curBuffer, 0);
		CD3DX12_TEXTURE_COPY_LOCATION dest(frameTex, 0);
		commandList->CopyTextureRegion(&dest, _outputRect.left, _outputRect.top, 0, &src, nullptr);
	}

	{
		CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
			frameTex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT, 0);
		commandList->ResourceBarrier(1, &barrier);
	}

	hr = commandList->Close();
	if (FAILED(hr)) {
		Logger::Get().ComError("ID3D12GraphicsCommandList::Close 失败", hr);
		return hr;
	}

	ID3D12CommandQueue* commandQueue = _graphicsContext.GetCommandQueue();
	commandQueue->ExecuteCommandLists(1, CommandListCast(&commandList));

	hr =_frameProducer->ConsumerEndFrame(commandQueue, fenceValueToSignal);
	if (FAILED(hr)) {
		Logger::Get().ComError("FrameProducer::ConsumerEndFrame 失败", hr);
		return hr;
	}

	hr = _presenter->EndFrame(waitForGpu);
	if (FAILED(hr)) {
		Logger::Get().ComError("SwapChainPresenter::EndFrame 失败", hr);
		return hr;
	}

	// GraphicsContext::EndFrame 必须在 SwapChain::EndFrame 之后
	hr = _graphicsContext.EndFrame();
	if (FAILED(hr)) {
		Logger::Get().ComError("GraphicsContext::EndFrame 失败", hr);
		return hr;
	}

	return S_OK;
}

void Renderer2::_UpdateOutputRect(Size outputSize) noexcept {
	// TODO: 窗口模式缩放始终填充画面
	const Size rendererSize = _presenter->GetSize();
	OutputAlignment alignment = ScalingWindow::Get().Options().outputAlignment;

	using enum OutputAlignment;

	if (alignment == LeftTop || alignment == Left || alignment == LeftBottom) {
		_outputRect.left = 0;
		_outputRect.right = outputSize.width;
	} else if (alignment == Top || alignment == Center || alignment == Bottom) {
		_outputRect.left = (rendererSize.width - outputSize.width) / 2;
		_outputRect.right = _outputRect.left + outputSize.width;
	} else {
		_outputRect.left = rendererSize.width - outputSize.width;
		_outputRect.right = rendererSize.width;
	}

	if (alignment == LeftTop || alignment == Top || alignment == RightTop) {
		_outputRect.top = 0;
		_outputRect.bottom = outputSize.height;
	} else if (alignment == Left || alignment == Center || alignment == Right) {
		_outputRect.top = (rendererSize.height - outputSize.height) / 2;
		_outputRect.bottom = _outputRect.top + outputSize.height;
	} else {
		_outputRect.top = rendererSize.height - outputSize.height;
		_outputRect.bottom = rendererSize.height;
	}

	assert(_outputRect.left + (LONG)outputSize.width == _outputRect.right);
	assert(_outputRect.top + (LONG)outputSize.height == _outputRect.bottom);
}

bool Renderer2::_CheckResult(bool success, std::string_view errorMsg) noexcept {
	assert(_state == ComponentState::NoError);

	if (!success) {
		_state = ComponentState::Error;
		Logger::Get().Error(errorMsg);
	}
	return success;
}

bool Renderer2::_CheckResult(HRESULT hr, std::string_view errorMsg) noexcept {
	assert(_state == ComponentState::NoError);

	if (SUCCEEDED(hr)) {
		return true;
	}

	if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
		_state = ComponentState::DeviceLost;
	} else {
		_state = ComponentState::Error;
	}

	Logger::Get().ComError(errorMsg, hr);
	return false;
}

}

#include "pch.h"
#include "Renderer2.h"
#include "CommonSharedConstants.h"
#include "FrameProducer.h"
#include "Logger.h"
#include "ScalingWindow.h"
#include "SrcTracker.h"
#include "SwapChainPresenter.h"
#include <windows.graphics.display.interop.h>

namespace Magpie {

static constexpr HRESULT S_RECOVERED = CommonSharedConstants::S_RECOVERED;

static constexpr float SCENE_REFERRED_SDR_WHITE_LEVEL = (float)CommonSharedConstants::SCENE_REFERRED_SDR_WHITE_LEVEL;

Renderer2::Renderer2() noexcept {}

Renderer2::~Renderer2() noexcept {
	_graphicsContext.WaitForGPU();
}

ScalingError Renderer2::Initialize(
	HWND hwndAttach,
	HMONITOR hMonitor,
	Size size,
	const RECT& srcRect,
	OverlayOptions& /*overlayOptions*/
) noexcept {
	_hCurMonitor = hMonitor;

	const ScalingOptions& options = ScalingWindow::Get().Options();
	if (FAILED(_graphicsContext.Initialize(options.graphicsCardId, options.Is3DGameMode() ? 4 : 8, D3D12_COMMAND_LIST_TYPE_DIRECT))) {
		Logger::Get().Error("初始化 GraphicsContext 失败");
		return ScalingError::ScalingFailedGeneral;
	}

	// 失败则回落到使用传统方法获取颜色显示能力
	_TryInitDisplayInfo();

	if (!_UpdateColorInfo()) {
		Logger::Get().Error("_UpdateColorInfo 失败");
		return ScalingError::ScalingFailedGeneral;
	}

	_frameProducer = std::make_unique<FrameProducer>();
	// 也会初始化 SharedRingBuffer
	_frameProducer->InitializeAsync(
		_graphicsContext.GetDevice(), _sharedRingBuffer, srcRect, hMonitor, _colorInfo);

	_presenter = std::make_unique<SwapChainPresenter>();
	if (!_presenter->Initialize(_graphicsContext, hwndAttach, size, _colorInfo)) {
		Logger::Get().Error("初始化 SwapChainPresenter 失败");
		return ScalingError::ScalingFailedGeneral;
	}

	uint32_t frameWidth;
	uint32_t frameHeight;
	if (!_frameProducer->WaitForInitialize(frameWidth, frameHeight)) {
		Logger::Get().Error("FrameProducer::WaitForInitialize 失败");
		return ScalingError::ScalingFailedGeneral;
	}

	_outputRect.right = frameWidth;
	_outputRect.bottom = frameHeight;

	return ScalingError::NoError;
}

ComponentState Renderer2::Render(bool /*force*/, bool /*waitForGpu*/) noexcept {
	if (_state != ComponentState::NoError) {
		return _state;
	}

	ID3D12Resource* curBuffer;
	D3D12_RESOURCE_STATES bufferState;
	ID3D12Fence1* fenceToSignal;
	UINT64 fenceValueToSignal;
	if (!_sharedRingBuffer.ConsumerBeginFrame(curBuffer, bufferState, fenceToSignal, fenceValueToSignal, D3D12_RESOURCE_STATE_COPY_SOURCE)) {
		return _state;
	}

	// SwapChain::BeginFrame 和 GraphicsContext::BeginFrame 无顺序要求，不过
	// 前者通常等待时间更久，将它放在前面可以减少等待次数。
	ID3D12Resource* frameTex;
	CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle;
	_presenter->BeginFrame(&frameTex, rtvHandle);

	uint32_t frameIndex;
	if (!_CheckResult(_graphicsContext.BeginFrame(frameIndex, nullptr), "GraphicsContext::BeginFrame 失败")) {
		return _state;
	}

	ID3D12GraphicsCommandList* commandList = _graphicsContext.GetCommandList();

	if (const Size size = _presenter->Size(); _outputRect == RECT{ 0,0,(LONG)size.width,(LONG)size.height }) {
		if (bufferState == D3D12_RESOURCE_STATE_COPY_SOURCE) {
			CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
				frameTex, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST, 0);
			commandList->ResourceBarrier(1, &barrier);
		} else {
			CD3DX12_RESOURCE_BARRIER barriers[] = {
				CD3DX12_RESOURCE_BARRIER::Transition(
					frameTex, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST, 0),
				CD3DX12_RESOURCE_BARRIER::Transition(
					curBuffer, bufferState, D3D12_RESOURCE_STATE_COPY_SOURCE, 0)
			};
			commandList->ResourceBarrier((UINT)std::size(barriers), barriers);
		}

		commandList->CopyResource(frameTex, curBuffer);
	} else {
		D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
			frameTex, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET, 0);
		commandList->ResourceBarrier(1, &barrier);

		// 存在黑边时应填充背景。使用交换链呈现时需要这个操作，因为我们指定了 
		// DXGI_SWAP_EFFECT_FLIP_DISCARD，同时也是为了和 RTSS 兼容。
		static constexpr FLOAT BLACK[4] = { 0.0f,0.0f,0.0f,1.0f };
		commandList->ClearRenderTargetView(rtvHandle, BLACK, 0, nullptr);

		if (bufferState == D3D12_RESOURCE_STATE_COPY_SOURCE) {
			barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
			barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
			commandList->ResourceBarrier(1, &barrier);
		} else {
			CD3DX12_RESOURCE_BARRIER barriers[] = {
				CD3DX12_RESOURCE_BARRIER::Transition(
					frameTex, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST, 0),
				CD3DX12_RESOURCE_BARRIER::Transition(
					curBuffer, bufferState, D3D12_RESOURCE_STATE_COPY_SOURCE, 0)
			};
			commandList->ResourceBarrier((UINT)std::size(barriers), barriers);
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

	if (!_CheckResult(commandList->Close(), "ID3D12GraphicsCommandList::Close 失败")) {
		return _state;
	}

	ID3D12CommandQueue* commandQueue = _graphicsContext.GetCommandQueue();
	commandQueue->ExecuteCommandLists(1, CommandListCast(&commandList));

	if (!_CheckResult(commandQueue->Signal(fenceToSignal, fenceValueToSignal), "ID3D12CommandQueue::Signal 失败")) {
		return _state;
	}

	if (!_CheckResult(_presenter->EndFrame(), "SwapChainPresenter::EndFrame 失败")) {
		return _state;
	}

	// GraphicsContext::EndFrame 必须在 SwapChain::EndFrame 之后
	_CheckResult(_graphicsContext.EndFrame(), "GraphicsContext::EndFrame 失败");
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

void Renderer2::OnSizeChanged(Size size) noexcept {
	if (_state == ComponentState::NoError) {
		_CheckResult(_presenter->OnSizeChanged(size), "SwapChainPresenter::OnSizeChanged 失败");
	}
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

void Renderer2::OnMsgDisplayChanged() noexcept {
	// winrt::DisplayInformation 可用时已通过事件监听颜色配置变化
	if (_state != ComponentState::NoError || _displayInfo) {
		return;
	}

	_CheckResult(_UpdateColorSpace(), "_UpdateColorSpace 失败");
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
	return E_NOTIMPL;
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

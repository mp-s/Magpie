#include "pch.h"
#include "Renderer2.h"
#include "DebugInfo.h"
#include "FrameProducer.h"
#include "Logger.h"
#include "ScalingWindow.h"
#include "SwapChainPresenter.h"
#include "shaders/CopyFrameVS.h"
#include "shaders/SimplePS.h"
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
	const RECT& rendererRect,
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
		D3D12_COMMAND_LIST_TYPE_DIRECT,
		_dynamicDescriptorHeap
	)) {
		Logger::Get().Error("初始化 GraphicsContext 失败");
		return ScalingError::ScalingFailedGeneral;
	}

	ID3D12Device5* device = _graphicsContext.GetDevice();

#ifdef MP_DEBUG_INFO
	{
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

	const Size rendererSize = {
		uint32_t(rendererRect.right - rendererRect.left),
		uint32_t(rendererRect.bottom - rendererRect.top)
	};

	Size outputSize;
	SimpleTask<bool> task;
	_frameProducer.InitializeAsync(
		_graphicsContext, _colorInfo, hMonitor, srcRect, rendererSize, outputSize, task);

	_presenter = std::make_unique<SwapChainPresenter>();
	if (!_presenter->Initialize(_graphicsContext, hwndAttach, rendererSize, _colorInfo)) {
		Logger::Get().Error("SwapChainPresenter::Initialize 失败");
		return ScalingError::ScalingFailedGeneral;
	}

	{
		winrt::com_ptr<ID3DBlob> signature;

		CD3DX12_DESCRIPTOR_RANGE1 srvRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0,
			D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE);
		D3D12_ROOT_PARAMETER1 rootParams[] = {
			{
				.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
				.Constants = {
					.Num32BitValues = 4
				},
				.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX
			},
			{
				.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
				.DescriptorTable = {
					.NumDescriptorRanges = 1,
					.pDescriptorRanges = &srvRange
				}
			}
		};
		D3D12_STATIC_SAMPLER_DESC samplerDesc = {
			.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT,
			.AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			.AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER,
			// 边界外使用黑色填充
			.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK,
			.ShaderRegister = 0
		};
		CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc(
			(UINT)std::size(rootParams), rootParams, 1, &samplerDesc, D3D12_ROOT_SIGNATURE_FLAG_NONE);

		HRESULT hr = D3DX12SerializeVersionedRootSignature(
			&rootSignatureDesc, _graphicsContext.GetRootSignatureVersion(), signature.put(), nullptr);
		if (FAILED(hr)) {
			return ScalingError::ScalingFailedGeneral;
		}

		hr = device->CreateRootSignature(
			0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&_rootSignature));
		if (FAILED(hr)) {
			return ScalingError::ScalingFailedGeneral;
		}

		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {
			.pRootSignature = _rootSignature.get(),
			.VS = CD3DX12_SHADER_BYTECODE(CopyFrameVS, sizeof(CopyFrameVS)),
			.PS = CD3DX12_SHADER_BYTECODE(SimplePS, sizeof(SimplePS)),
			.BlendState = {
				.RenderTarget = {{ .RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL }}
			},
			.SampleMask = UINT_MAX,
			.RasterizerState = {
				.FillMode = D3D12_FILL_MODE_SOLID,
				.CullMode = D3D12_CULL_MODE_NONE
			},
			.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE,
			.NumRenderTargets = 1,
			.RTVFormats = { _colorInfo.kind == winrt::AdvancedColorKind::StandardDynamicRange ?
				DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R16G16B16A16_FLOAT },
			.SampleDesc = { .Count = 1 }
		};
		hr = device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&_pipelineState));
		if (FAILED(hr)) {
			return ScalingError::ScalingFailedGeneral;
		}
	}

	// 同步点，使生产者线程的修改可见
	if (!task.GetResult(std::memory_order_acquire)) {
		Logger::Get().Error("FrameProducer::InitializeAsync 失败");
		return ScalingError::ScalingFailedGeneral;
	}

	_UpdateOutputRect(outputSize);

	const RECT destRect = {
		rendererRect.left + (LONG)_outputRect.left,
		rendererRect.top + (LONG)_outputRect.top,
		rendererRect.left + (LONG)_outputRect.right,
		rendererRect.top + (LONG)_outputRect.bottom
	};
	if (!_cursorDrawer.Initialize(_graphicsContext, destRect)) {
		Logger::Get().Error("CursorDrawer2::Initialize 失败");
		return ScalingError::ScalingFailedGeneral;
	}

	return ScalingError::NoError;
}

ComponentState Renderer2::Render(
	HCURSOR hCursor,
	POINT cursorPos,
	bool waitForGpu,
	bool* waitingForFirstFrame
) noexcept {
	assert(!waitingForFirstFrame || !*waitingForFirstFrame);

	if (_state != ComponentState::NoError) {
		return _state;
	}

	_state = _frameProducer.GetState();
	if (_state != ComponentState::NoError) {
		return _state;
	}

	{
		const uint64_t latestProducerFrameNumber = _frameProducer.GetLatestFrameNumber();
		if (latestProducerFrameNumber == 0) {
			if (waitingForFirstFrame && latestProducerFrameNumber == 0) {
				*waitingForFirstFrame = true;
			}
			return _state;
		}

		if (latestProducerFrameNumber == _lastProducerFrameNumber &&
			!_cursorDrawer.CheckForRedraw(hCursor, cursorPos)) {
			return _state;
		}

		_lastProducerFrameNumber = latestProducerFrameNumber;
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

	// 确保消费者不再使用环形缓冲区
	if (!_CheckResult(_graphicsContext.WaitForGpu(), "GraphicsContext::WaitForGpu 失败")) {
		return;
	}

	Size outputSize;
	SimpleTask<HRESULT> task;
	_frameProducer.OnResizedAsync(size, outputSize, task);

	if (!_CheckResult(_presenter->OnResized(size), "SwapChainPresenter::OnResized 失败")) {
		return;
	}

	if (!_CheckResult(task.GetResult(std::memory_order_acquire),
		"FrameProducer::OnResizedAsync 失败")) {
		return;
	}

	_UpdateOutputRect(outputSize);
}

void Renderer2::OnMoveStarted() noexcept {
	_cursorDrawer.OnMoveStarted();
}

void Renderer2::OnMoveEnded() noexcept {
	_cursorDrawer.OnMoveEnded();
}

void Renderer2::OnCursorVirtualizationStarted() noexcept {
	_cursorDrawer.OnCursorVirtualizationStarted();
}

void Renderer2::OnCursorVirtualizationEnded() noexcept {
	_cursorDrawer.OnCursorVirtualizationEnded();
}

void Renderer2::OnSrcMoveStarted() noexcept {
	_cursorDrawer.OnSrcMoveStarted();
}

void Renderer2::OnSrcMoveEnded() noexcept {
	_cursorDrawer.OnSrcMoveEnded();
}

void Renderer2::OnDestRectChanged(const RECT& destRect) noexcept {
	_cursorDrawer.OnDestRectChanged(destRect);
}

void Renderer2::OnMsgDisplayChanged() noexcept {
	// winrt::DisplayInformation 可用时已通过事件监听颜色配置变化
	if (_state != ComponentState::NoError || _displayInfo) {
		return;
	}

	_CheckResult(_UpdateColorSpace(), "_UpdateColorSpace 失败");
}

void Renderer2::OnCursorVisibilityChanged(bool isVisible, bool onDestory) noexcept {
	_frameProducer.OnCursorVisibilityChanged(isVisible, onDestory);
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

	// 确保消费者不再使用环形缓冲区
	HRESULT hr = _graphicsContext.WaitForGpu();
	if (FAILED(hr)) {
		Logger::Get().ComError("GraphicsContext::WaitForGpu 失败", hr);
		return hr;
	}

	SimpleTask<HRESULT> task;
	_frameProducer.OnColorInfoChangedAsync(_colorInfo, task);

	hr = _presenter->OnColorInfoChanged(_colorInfo);
	if (FAILED(hr)) {
		Logger::Get().ComError("SwapChainPresenter::OnColorInfoChanged 失败", hr);
		return hr;
	}

	hr = task.GetResult();
	if (FAILED(hr)) {
		Logger::Get().ComError("FrameProducer::OnColorInfoChangedAsync 失败", hr);
		return hr;
	}

	// 生产者渲染完成会通知消费者渲染新帧
	return S_OK;
}

HRESULT Renderer2::_RenderImpl(bool waitForGpu) noexcept {
	// 处于 COMMON 状态，依赖隐式状态转换
	ID3D12Resource* curBuffer;
	uint32_t curBufferSrvIdx;
	UINT64 fenceValueToSignal;
	ID3D12DescriptorHeap* heap;
	D3D12_GPU_DESCRIPTOR_HANDLE heapGpuHandle;
	if (!_frameProducer.ConsumerBeginFrame(
		curBuffer, curBufferSrvIdx, fenceValueToSignal, heap, heapGpuHandle)) {
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
	HRESULT hr = _graphicsContext.BeginFrame(frameIndex, _pipelineState.get());
	if (FAILED(hr)) {
		Logger::Get().ComError("GraphicsContext::BeginFrame 失败", hr);
		return hr;
	}

	ID3D12GraphicsCommandList* commandList = _graphicsContext.GetCommandList();

	commandList->SetDescriptorHeaps(1, &heap);

	commandList->SetGraphicsRootSignature(_rootSignature.get());

	const Size rendererSize = _presenter->GetSize();

	{
		float outputWidth = float(_outputRect.right - _outputRect.left);
		float outputHeight = float(_outputRect.bottom - _outputRect.top);
		// 这些参数将输出区域 uv 变换为 0~1
		float constants[] = {
			rendererSize.width / outputWidth,
			rendererSize.height / outputHeight,
			_outputRect.left / -outputWidth,
			_outputRect.top / -outputHeight
		};
		commandList->SetGraphicsRoot32BitConstants(0, (UINT)std::size(constants), constants, 0);
	}

	auto& dynamicDescriptorHeap = _graphicsContext.GetDynamicDescriptorHeap();
	const uint32_t descriptorSize = dynamicDescriptorHeap.GetDescriptorSize();

	commandList->SetGraphicsRootDescriptorTable(
		1, CD3DX12_GPU_DESCRIPTOR_HANDLE(heapGpuHandle, curBufferSrvIdx, descriptorSize));

	{
		CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
			frameTex, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET, 0);
		commandList->ResourceBarrier(1, &barrier);
	}

	{
		CD3DX12_VIEWPORT viewport(0.0f, 0.0f, (float)rendererSize.width, (float)rendererSize.height);
		commandList->RSSetViewports(1, &viewport);
	}
	{
		CD3DX12_RECT scissorRect(0, 0, (LONG)rendererSize.width, (LONG)rendererSize.height);
		commandList->RSSetScissorRects(1, &scissorRect);
	}

	commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

	commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	commandList->DrawInstanced(3, 1, 0, 0);

	hr = _cursorDrawer.Draw();
	if (FAILED(hr)) {
		Logger::Get().ComError("CursorDrawer2::Draw 失败", hr);
		return hr;
	}

	{
		CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
			frameTex, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT, 0);
		commandList->ResourceBarrier(1, &barrier);
	}

	hr = commandList->Close();
	if (FAILED(hr)) {
		Logger::Get().ComError("ID3D12GraphicsCommandList::Close 失败", hr);
		return hr;
	}

	ID3D12CommandQueue* commandQueue = _graphicsContext.GetCommandQueue();
	commandQueue->ExecuteCommandLists(1, CommandListCast(&commandList));

	hr =_frameProducer.ConsumerEndFrame(commandQueue, fenceValueToSignal);
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

	assert(_outputRect.left + outputSize.width == _outputRect.right);
	assert(_outputRect.top + outputSize.height == _outputRect.bottom);
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

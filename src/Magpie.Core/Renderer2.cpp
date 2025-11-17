#include "pch.h"
#include "CommonSharedConstants.h"
#include "DirectXHelper.h"
#include "Logger.h"
#include "Renderer2.h"
#include "ScalingWindow.h"
#include "StrHelper.h"
#include "SwapChainPresenter.h"
#include "SrcTracker.h"
#include <d3dkmthk.h>
#include <dispatcherqueue.h>
#include <dxgidebug.h>
#include <windows.graphics.display.interop.h>

namespace Magpie {

static constexpr HRESULT S_RECOVERED = CommonSharedConstants::S_RECOVERED;

static constexpr float SCENE_REFERRED_SDR_WHITE_LEVEL = (float)CommonSharedConstants::SCENE_REFERRED_SDR_WHITE_LEVEL;

// 为生产队列和消费队列设置不同的 GUID 以提高并发度
// {592345E2-622C-46F1-9A93-A87AF9C04F22}
static const winrt::guid COSUMER_QUEUE_ID(0x592345e2, 0x622c, 0x46f1, { 0x9a, 0x93, 0xa8, 0x7a, 0xf9, 0xc0, 0x4f, 0x22 });
// {A4AEA0B1-A73D-4224-BE15-4B82E04B2E2A}
// static const winrt::guid PRODUCER_QUEUE_ID(0xa4aea0b1, 0xa73d, 0x4224, { 0xbe, 0x15, 0x4b, 0x82, 0xe0, 0x4b, 0x2e, 0x2a });

Renderer2::Renderer2() noexcept {}

Renderer2::~Renderer2() noexcept {
	if (_producerThread.joinable()) {
		const HANDLE hThread = _producerThread.native_handle();

		if (!wil::handle_wait(hThread, 0)) {
			const DWORD threadId = GetThreadId(_producerThread.native_handle());

			// 持续尝试直到 _producerThread 创建了消息队列
			while (!PostThreadMessage(threadId, WM_QUIT, 0, 0)) {
				if (wil::handle_wait(hThread, 1)) {
					break;
				}
			}
		}

		_producerThread.join();
	}
}

ScalingError Renderer2::Initialize(HWND hwndAttach, OverlayOptions& /*overlayOptions*/) noexcept {
	[[maybe_unused]] static Ignore _ = [] {
#ifdef _DEBUG
		winrt::com_ptr<IDXGIInfoQueue> dxgiInfoQueue;
		if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&dxgiInfoQueue)))) {
			// 发生错误时中断
			dxgiInfoQueue->SetBreakOnSeverity(DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_CORRUPTION, true);
			dxgiInfoQueue->SetBreakOnSeverity(DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_ERROR, true);
		}

		{
			winrt::com_ptr<ID3D12Debug1> debugController;
			if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
				debugController->EnableDebugLayer();
				// 启用 GPU-based validation，但会产生警告消息，而且这个消息无法轻易禁用
				debugController->SetEnableGPUBasedValidation(TRUE);

				// Win11 开始支持生成默认名字，包含资源的基本属性
				if (winrt::com_ptr<ID3D12Debug5> debugController5 = debugController.try_as<ID3D12Debug5>()) {
					debugController5->SetEnableAutoName(TRUE);
				}
			}
		}
#endif
		// 声明支持 TDR 恢复
		DXGIDeclareAdapterRemovalSupport();

		return Ignore();
	}();

	if (FAILED(_CreateDXGIFactory())) {
		Logger::Get().Error("_CreateDXGIFactory 失败");
		return ScalingError::ScalingFailedGeneral;
	}

	if (!_CreateAdapterAndDevice(ScalingWindow::Get().Options().graphicsCardId)) {
		Logger::Get().Error("找不到可用的图形适配器");
		return ScalingError::ScalingFailedGeneral;
	}

	// 检查半精度浮点支持
	{
		D3D12_FEATURE_DATA_D3D12_OPTIONS featureData{};
		HRESULT hr = _device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &featureData, sizeof(featureData));
		if (SUCCEEDED(hr)) {
			_isFP16Supported = featureData.MinPrecisionSupport & D3D12_SHADER_MIN_PRECISION_SUPPORT_16_BIT;
			Logger::Get().Info(StrHelper::Concat("FP16 支持: ", _isFP16Supported ? "是" : "否"));
		} else {
			Logger::Get().ComError("CheckFeatureSupport 失败", hr);
		}
	}

	_TryInitDisplayInfo();

	if (FAILED(_UpdateAdvancedColor(true))) {
		return ScalingError::ScalingFailedGeneral;
	}

	// 创建 D3D 设备后开始初始化生产者线程
	_producerThread = std::thread(&Renderer2::_ProducerThreadProc, this);

	{
		D3D12_COMMAND_QUEUE_DESC queueDesc{
			.Type = D3D12_COMMAND_LIST_TYPE_DIRECT,
			.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE
		};
		if (winrt::com_ptr<ID3D12Device9> device9 = _device.try_as<ID3D12Device9>()) {
			// 设置 CreatorID 可以提高并发度
			HRESULT hr = device9->CreateCommandQueue1(&queueDesc, COSUMER_QUEUE_ID, IID_PPV_ARGS(&_consumerCommandQueue));
			if (FAILED(hr)) {
				Logger::Get().ComError("CreateCommandQueue1 失败", hr);
				return ScalingError::ScalingFailedGeneral;
			}
		} else {
			HRESULT hr = _device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&_consumerCommandQueue));
			if (FAILED(hr)) {
				Logger::Get().ComError("CreateCommandQueue 失败", hr);
				return ScalingError::ScalingFailedGeneral;
			}
		}
	}

	HRESULT hr = _device->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
		D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(&_consumerCommandList));
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateCommandList1 失败", hr);
		return ScalingError::ScalingFailedGeneral;
	}

	_presenter = std::make_unique<SwapChainPresenter>();
	if (!_presenter->Initialize(_device.get(), _consumerCommandQueue.get(), _dxgiFactory.get(),
		hwndAttach, _curAcKind != winrt::AdvancedColorKind::StandardDynamicRange)) {
		Logger::Get().Error("初始化 SwapChainPresenter 失败");
		return ScalingError::ScalingFailedGeneral;
	}

	_consumerCommandAllocators.resize(_presenter->GetBufferCount());
	for (winrt::com_ptr<ID3D12CommandAllocator>& commandAllocator : _consumerCommandAllocators) {
		if (FAILED(_device->CreateCommandAllocator(
			D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocator)))) {
			return ScalingError::ScalingFailedGeneral;
		}
	}

	return ScalingError::NoError;
}

bool Renderer2::Render(bool /*force*/, bool /*waitForGpu*/, bool onHandlingDeviceLost) noexcept {
	ID3D12Resource* frameTex;
	CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle;
	uint32_t frameIndex;
	HRESULT hr = _CheckDeviceLost(_presenter->BeginFrame(&frameTex, rtvHandle, frameIndex), onHandlingDeviceLost);
	if (FAILED(hr) || hr == S_RECOVERED) {
		return hr == S_RECOVERED;
	}

	hr = _CheckDeviceLost(_consumerCommandAllocators[frameIndex]->Reset(), onHandlingDeviceLost);
	if (FAILED(hr) || hr == S_RECOVERED) {
		return hr == S_RECOVERED;
	}

	hr = _CheckDeviceLost(_consumerCommandList->Reset(
		_consumerCommandAllocators[frameIndex].get(), nullptr), onHandlingDeviceLost);
	if (FAILED(hr) || hr == S_RECOVERED) {
		return hr == S_RECOVERED;
	}

	{
		CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
			frameTex, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
		_consumerCommandList->ResourceBarrier(1, &barrier);
	}

	{
		const float clearColor[] = { 0.8f, 0.8f, 0.6f, 1.0f };
		_consumerCommandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
	}

	{
		CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
			frameTex, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
		_consumerCommandList->ResourceBarrier(1, &barrier);
	}

	hr = _CheckDeviceLost(_consumerCommandList->Close(), onHandlingDeviceLost);
	if (FAILED(hr) || hr == S_RECOVERED) {
		return hr == S_RECOVERED;
	}

	{
		ID3D12CommandList* t = _consumerCommandList.get();
		_consumerCommandQueue->ExecuteCommandLists(1, &t);
	}

	return SUCCEEDED(_CheckDeviceLost(_presenter->EndFrame(), onHandlingDeviceLost));
}

bool Renderer2::OnSizeChanged() noexcept {
	return SUCCEEDED(_CheckDeviceLost(_presenter->RecreateBuffers(
		_curAcKind != winrt::AdvancedColorKind::StandardDynamicRange)));
}

bool Renderer2::OnResizeEnded() noexcept {
	return SUCCEEDED(_CheckDeviceLost(_presenter->OnResizeEnded()));
}

bool Renderer2::OnSrcMonitorChanged() noexcept {
	_acInfoChangedRevoker.revoke();
	_displayInfo = nullptr;

	_TryInitDisplayInfo();
	_UpdateAdvancedColor(false, true);

	return true;
}

bool Renderer2::OnDisplayChanged() noexcept {
	return _displayInfo || SUCCEEDED(_CheckDeviceLost(_UpdateAdvancedColor()));
}

HRESULT Renderer2::_CreateDXGIFactory() noexcept {
	UINT dxgiFactoryFlags = 0;
#ifdef _DEBUG
	dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
#endif
	HRESULT hr = CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&_dxgiFactory));
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateDXGIFactory2 失败", hr);
	}
	return hr;
}

bool Renderer2::_CreateAdapterAndDevice(GraphicsCardId graphicsCardId) noexcept {
	winrt::com_ptr<IDXGIAdapter1> adapter;
	// 记录不支持 D3D12 的显卡索引，防止重复尝试
	int failedIdx = -1;

	if (graphicsCardId.idx >= 0) {
		assert(graphicsCardId.vendorId != 0 && graphicsCardId.deviceId != 0);

		// 先使用索引
		HRESULT hr = _dxgiFactory->EnumAdapters1(graphicsCardId.idx, adapter.put());
		if (SUCCEEDED(hr)) {
			DXGI_ADAPTER_DESC1 desc;
			hr = adapter->GetDesc1(&desc);
			if (SUCCEEDED(hr)) {
				if (desc.VendorId == graphicsCardId.vendorId && desc.DeviceId == graphicsCardId.deviceId) {
					if (_TryCreateD3DDevice(adapter, desc)) {
						return true;
					}

					failedIdx = graphicsCardId.idx;
					Logger::Get().Warn("用户指定的显示卡不支持 D3D12");
				} else {
					Logger::Get().Warn("显卡配置已变化");
				}
			}
		}

		// 如果已确认该显卡不支持 D3D12，不再重复尝试
		if (failedIdx == -1) {
			// 枚举查找 vendorId 和 deviceId 匹配的显卡
			for (UINT adapterIdx = 0;
				SUCCEEDED(_dxgiFactory->EnumAdapters1(adapterIdx, adapter.put()));
				++adapterIdx
			) {
				if ((int)adapterIdx == graphicsCardId.idx) {
					// 已经检查了 graphicsCardId.idx
					continue;
				}

				DXGI_ADAPTER_DESC1 desc;
				hr = adapter->GetDesc1(&desc);
				if (FAILED(hr)) {
					continue;
				}

				if (desc.VendorId == graphicsCardId.vendorId && desc.DeviceId == graphicsCardId.deviceId) {
					if (_TryCreateD3DDevice(adapter, desc)) {
						return true;
					}

					failedIdx = (int)adapterIdx;
					Logger::Get().Warn("用户指定的显示卡不支持 D3D12");
					break;
				}
			}
		}
	}

	// 枚举查找第一个支持 D3D12 的显卡
	for (UINT adapterIdx = 0;
		SUCCEEDED(_dxgiFactory->EnumAdapters1(adapterIdx, adapter.put()));
		++adapterIdx
	) {
		if ((int)adapterIdx == failedIdx) {
			// 无需再次尝试
			continue;
		}

		DXGI_ADAPTER_DESC1 desc;
		HRESULT hr = adapter->GetDesc1(&desc);
		if (FAILED(hr) || DirectXHelper::IsWARP(desc)) {
			continue;
		}

		if (_TryCreateD3DDevice(adapter, desc)) {
			return true;
		}
	}

	// 作为最后手段，回落到 CPU 渲染 (WARP)
	// https://docs.microsoft.com/en-us/windows/win32/direct3darticles/directx-warp
	HRESULT hr = _dxgiFactory->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
	if (FAILED(hr)) {
		Logger::Get().ComError("EnumWarpAdapter 失败", hr);
		return false;
	}

	DXGI_ADAPTER_DESC1 desc;
	hr = adapter->GetDesc1(&desc);
	if (FAILED(hr) || !_TryCreateD3DDevice(adapter, desc)) {
		Logger::Get().Error("创建 WARP 设备失败");
		return false;
	}

	return true;
}

static void SetGpuPriority() noexcept {
	// 来自 https://github.com/obsproject/obs-studio/blob/16cb051a57bb357fe866252c1360ce2c38e2deec/libobs-d3d11/d3d11-subsystem.cpp#L429
	// 不使用 REALTIME 优先级，它会造成系统不稳定，而且可能会导致源窗口卡顿。
	// OBS 还调用了 SetGPUThreadPriority，但这个接口似乎无用。
	NTSTATUS status = D3DKMTSetProcessSchedulingPriorityClass(
		GetCurrentProcess(), D3DKMT_SCHEDULINGPRIORITYCLASS_HIGH);
	if (status != STATUS_SUCCESS) {
		Logger::Get().NTError("D3DKMTSetProcessSchedulingPriorityClass 失败", status);
	}
}

bool Renderer2::_TryCreateD3DDevice(const winrt::com_ptr<IDXGIAdapter1>& adapter, const DXGI_ADAPTER_DESC1& adapterDesc) noexcept {
	HRESULT hr = D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&_device));
	if (FAILED(hr)) {
		Logger::Get().ComError("D3D12CreateDevice 失败", hr);
		return false;
	}

	{
		D3D_FEATURE_LEVEL featureLevels[] = {
			D3D_FEATURE_LEVEL_12_2,
			D3D_FEATURE_LEVEL_12_1,
			D3D_FEATURE_LEVEL_12_0,
			D3D_FEATURE_LEVEL_11_1,
			D3D_FEATURE_LEVEL_11_0
		};
		D3D12_FEATURE_DATA_FEATURE_LEVELS featureData{
			.NumFeatureLevels = (UINT)std::size(featureLevels),
			.pFeatureLevelsRequested = featureLevels
		};
		hr = _device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &featureData, sizeof(featureData));
		if (SUCCEEDED(hr)) {
			std::string_view flStr;
			switch (featureData.MaxSupportedFeatureLevel) {
			case D3D_FEATURE_LEVEL_12_2:
				flStr = "12.2";
				break;
			case D3D_FEATURE_LEVEL_12_1:
				flStr = "12.1";
				break;
			case D3D_FEATURE_LEVEL_12_0:
				flStr = "12.0";
				break;
			case D3D_FEATURE_LEVEL_11_1:
				flStr = "11.1";
				break;
			case D3D_FEATURE_LEVEL_11_0:
				flStr = "11.0";
				break;
			default:
				flStr = "未知";
				break;
			}
			Logger::Get().Info(fmt::format("已创建 D3D 设备\n\t功能级别: {}", flStr));
		} else {
			Logger::Get().ComError("CheckFeatureSupport 失败", hr);
		}
	}

	_dxgiAdapter = adapter.try_as<IDXGIAdapter4>();
	if (!_dxgiAdapter) {
		Logger::Get().Error("获取 IDXGIAdapter4 失败");
		return false;
	}

	_isUsingWarp = DirectXHelper::IsWARP(adapterDesc);

	Logger::Get().Info(fmt::format("当前图形适配器: \n\tVendorId: {:#x}\n\tDeviceId: {:#x}\n\tDescription: {}",
		adapterDesc.VendorId, adapterDesc.DeviceId, StrHelper::UTF16ToUTF8(adapterDesc.Description)));

	// 每次创建 D3D 设备后尝试提高 GPU 优先级，OBS 也是这么做的
	SetGpuPriority();

	return true;
}

void Renderer2::_TryInitDisplayInfo() noexcept {
	winrt::com_ptr<IDisplayInformationStaticsInterop> interop =
		winrt::try_get_activation_factory<winrt::DisplayInformation, IDisplayInformationStaticsInterop>();
	if (!interop) {
		return;
	}
	
	HRESULT hr = (interop->GetForMonitor(
		ScalingWindow::Get().SrcTracker().Monitor(),
		winrt::guid_of<winrt::DisplayInformation>(),
		winrt::put_abi(_displayInfo))
	);
	if (FAILED(hr)) {
		Logger::Get().ComError("IDisplayInformationStaticsInterop::GetForMonitor 失败", hr);
		return;
	}

	_acInfoChangedRevoker = _displayInfo.AdvancedColorInfoChanged(
		winrt::auto_revoke,
		[this](winrt::DisplayInformation const&, winrt::IInspectable const&) {
			_UpdateAdvancedColor();
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

HRESULT Renderer2::_UpdateAdvancedColorInfo() noexcept {
	if (_displayInfo) {
		winrt::AdvancedColorInfo acInfo = _displayInfo.GetAdvancedColorInfo();

		_curAcKind = acInfo.CurrentAdvancedColorKind();
		if (_curAcKind == winrt::AdvancedColorKind::HighDynamicRange) {
			_maxLuminance = acInfo.MaxLuminanceInNits() / SCENE_REFERRED_SDR_WHITE_LEVEL;
			_sdrWhiteLevel = acInfo.SdrWhiteLevelInNits() / SCENE_REFERRED_SDR_WHITE_LEVEL;
		} else {
			_maxLuminance = 1.0f;
			_sdrWhiteLevel = 1.0f;
		}

		return S_OK;
	}

	// 未找到视为 SDR
	_curAcKind = winrt::AdvancedColorKind::StandardDynamicRange;
	_maxLuminance = 1.0f;
	_sdrWhiteLevel = 1.0f;

	if (!_dxgiFactory->IsCurrent()) {
		HRESULT hr = _CreateDXGIFactory();
		if (FAILED(hr)) {
			return hr;
		}
	}

	const HMONITOR hCurMonitor = ScalingWindow::Get().SrcTracker().Monitor();
	winrt::com_ptr<IDXGIAdapter1> adapter;
	winrt::com_ptr<IDXGIOutput> output;
	for (UINT adapterIdx = 0;
		SUCCEEDED(_dxgiFactory->EnumAdapters1(adapterIdx, adapter.put()));
		++adapterIdx
	) {
		for (UINT outputIdx = 0;
			SUCCEEDED(adapter->EnumOutputs(outputIdx, output.put()));
			++outputIdx
		) {
			DXGI_OUTPUT_DESC1 desc;
			if (SUCCEEDED(output.try_as<IDXGIOutput6>()->GetDesc1(&desc))) {
				if (desc.Monitor == hCurMonitor) {
					// DXGI 将 WCG 视为 SDR
					if (desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) {
						_curAcKind = winrt::AdvancedColorKind::HighDynamicRange;
						_maxLuminance = desc.MaxLuminance / SCENE_REFERRED_SDR_WHITE_LEVEL;
						_sdrWhiteLevel = GetSDRWhiteLevel(desc.DeviceName);
					}

					return S_OK;
				}
			}
		}
	}

	return S_OK;
}

HRESULT Renderer2::_UpdateAdvancedColor(bool onInit, bool noRender) noexcept {
	winrt::AdvancedColorKind oldAcKind = _curAcKind;

	HRESULT hr = _UpdateAdvancedColorInfo();
	if (FAILED(hr)) {
		return hr;
	}

	if (!onInit && oldAcKind == _curAcKind) {
		return S_OK;
	}

	const bool shouldUpdateFrameBuffers =
		(oldAcKind == winrt::AdvancedColorKind::StandardDynamicRange) != (_curAcKind == winrt::AdvancedColorKind::StandardDynamicRange);

	if (!onInit && shouldUpdateFrameBuffers) {
		// 等待 GPU 完成然后改变交换链格式
		hr = _presenter->RecreateBuffers(
			_curAcKind != winrt::AdvancedColorKind::StandardDynamicRange);
		if (FAILED(hr)) {
			return hr;
		}
	}

	return (onInit || noRender || Render()) ? S_OK : E_FAIL;
}

void Renderer2::_ProducerThreadProc() noexcept {
#ifdef _DEBUG
	SetThreadDescription(GetCurrentThread(), L"Magpie-缩放生产者线程");
#endif

	winrt::init_apartment(winrt::apartment_type::single_threaded);

	if (!_InitProducer()) {
		return;
	}

	MSG msg;
	while (GetMessage(&msg, NULL, 0, 0)) {
		if (msg.message == WM_QUIT) {
			return;
		}

		DispatchMessage(&msg);
	}
}

bool Renderer2::_InitProducer() noexcept {
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
			return NULL;
		}

		_producerThreadDispatcher = dqc.DispatcherQueue();
	}

	return true;
}

// TODO: 处理设备丢失
HRESULT Renderer2::_CheckDeviceLost(HRESULT hr, bool /*onHandlingDeviceLost*/) noexcept {
	return hr;
	/*if (SUCCEEDED(hr)) {
		return hr;
	}

	// 处理设备丢失时再次发生设备丢失则不再尝试恢复
	if ((hr != DXGI_ERROR_DEVICE_REMOVED && hr != DXGI_ERROR_DEVICE_RESET) || onHandlingDeviceLost) {
		return hr;
	}

	// 设备丢失，需要重新初始化
	return _HandleDeviceLost() ? S_RECOVERED : hr;*/
}

}

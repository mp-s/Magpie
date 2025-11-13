#include "pch.h"
#include "Renderer2.h"
#include "Logger.h"
#include "DirectXHelper.h"
#include "ScalingWindow.h"
#include "StrHelper.h"
#include "SwapChainPresenter.h"
#include <d3dkmthk.h>
#include <dxgidebug.h>

namespace Magpie {

// 为生产队列和消费队列设置不同的 GUID 以提高并发度
// {592345E2-622C-46F1-9A93-A87AF9C04F22}
static const winrt::guid COSUMER_QUEUE_ID(0x592345e2, 0x622c, 0x46f1, { 0x9a, 0x93, 0xa8, 0x7a, 0xf9, 0xc0, 0x4f, 0x22 });
// {A4AEA0B1-A73D-4224-BE15-4B82E04B2E2A}
static const winrt::guid PRODUCER_QUEUE_ID(0xa4aea0b1, 0xa73d, 0x4224, { 0xbe, 0x15, 0x4b, 0x82, 0xe0, 0x4b, 0x2e, 0x2a });

Renderer2::Renderer2() noexcept {}

Renderer2::~Renderer2() noexcept {}

ScalingError Renderer2::Initialize(HWND /*hwndAttach*/, OverlayOptions& /*overlayOptions*/) noexcept {
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

	// 检查可变帧率支持
	BOOL supportTearing = FALSE;
	HRESULT hr = _dxgiFactory->CheckFeatureSupport(
		DXGI_FEATURE_PRESENT_ALLOW_TEARING, &supportTearing, sizeof(supportTearing));
	if (FAILED(hr)) {
		Logger::Get().ComWarn("CheckFeatureSupport 失败", hr);
	}

	_isTearingSupported = supportTearing;
	Logger::Get().Info(fmt::format("可变刷新率支持: {}", supportTearing ? "是" : "否"));

	if (!_CreateAdapterAndDevice(ScalingWindow::Get().Options().graphicsCardId)) {
		Logger::Get().Error("找不到可用的图形适配器");
		return ScalingError::ScalingFailedGeneral;
	}

	// 检查半精度浮点支持
	{
		D3D12_FEATURE_DATA_D3D12_OPTIONS featureData{};
		hr = _device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &featureData, sizeof(featureData));
		if (SUCCEEDED(hr)) {
			_isFP16Supported = featureData.MinPrecisionSupport & D3D12_SHADER_MIN_PRECISION_SUPPORT_16_BIT;
			Logger::Get().Info(StrHelper::Concat("FP16 支持: ", _isFP16Supported ? "是" : "否"));
		} else {
			Logger::Get().ComError("CheckFeatureSupport 失败", hr);
		}
	}

	{
		D3D12_COMMAND_QUEUE_DESC queueDesc{
			.Type = D3D12_COMMAND_LIST_TYPE_DIRECT,
			.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE
		};
		if (winrt::com_ptr<ID3D12Device9> device9 = _device.try_as<ID3D12Device9>()) {
			// 设置 CreatorID 可以提高并发度
			hr = device9->CreateCommandQueue1(&queueDesc, COSUMER_QUEUE_ID, IID_PPV_ARGS(&_consumerCommandQueue));
			if (FAILED(hr)) {
				Logger::Get().ComError("CreateCommandQueue1 失败", hr);
				return ScalingError::ScalingFailedGeneral;
			}
		} else {
			hr = _device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&_consumerCommandQueue));
			if (FAILED(hr)) {
				Logger::Get().ComError("CreateCommandQueue 失败", hr);
				return ScalingError::ScalingFailedGeneral;
			}
		}
	}

	_presenter = std::make_unique<SwapChainPresenter>();
	if (!_presenter->Initialize(hwndAttach, _frontendResources)) {
		Logger::Get().Error("初始化 AdaptivePresenter 失败");
		return ScalingError::ScalingFailedGeneral;
	}

	// _backendThread = std::thread(&Renderer2::_BackendThreadProc, this);

	return ScalingError::NoError;
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

void Renderer2::_BackendThreadProc() noexcept {
	
}

}

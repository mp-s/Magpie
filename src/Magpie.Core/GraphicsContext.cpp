#include "pch.h"
#include "GraphicsContext.h"
#include "DebugInfo.h"
#include "Logger.h"
#include "DirectXHelper.h"
#include "StrHelper.h"
#include "DescriptorHeap.h"

namespace Magpie {

bool GraphicsContext::Initialize(
	const GraphicsCardId& graphicsCardId,
	uint32_t maxInFlightFrameCount,
	D3D12_COMMAND_QUEUE_PRIORITY priority,
	D3D12_COMMAND_LIST_TYPE commandListType,
	DescriptorHeap& descriptorHeap,
	bool disableFrameFenceTracking
) noexcept {
	_descriptorHeap = &descriptorHeap;

	HRESULT hr = _CreateDXGIFactory();
	if (FAILED(hr)) {
		Logger::Get().ComError("_CreateDXGIFactory 失败", hr);
		return false;
	}

	if (!_CreateAdapterAndDevice(graphicsCardId)) {
		Logger::Get().Error("_CreateAdapterAndDevice 失败");
		return false;
	}

#ifdef _DEBUG
	// 调试层汇报错误或警告时中断
	if (winrt::com_ptr<ID3D12InfoQueue> infoQueue = _device.try_as<ID3D12InfoQueue>()) {
		infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
		infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
		infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, TRUE);
	}
#endif

	// 检查根签名版本
	{
		D3D12_FEATURE_DATA_ROOT_SIGNATURE featureData = { .HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1 };
		hr = _device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &featureData, sizeof(featureData));
		if (FAILED(hr)) {
			Logger::Get().ComError("CheckFeatureSupport 失败", hr);
			return false;
		}
		_rootSignatureVersion = featureData.HighestVersion;
	}

	// 检查是否是集成显卡
	{
		D3D12_FEATURE_DATA_ARCHITECTURE1 data{};
		if (SUCCEEDED(_device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE1, &data, sizeof(data)))) {
			_isUMA = data.UMA;
		}
	}

	// 检查 Resizable BAR 支持
	{
		D3D12_FEATURE_DATA_D3D12_OPTIONS16 data{};
		if (SUCCEEDED(_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS16, &data, sizeof(data)))) {
			_isGPUUploadHeapSupported = data.GPUUploadHeapSupported;
		}
	}

	// 检查 shader model 6.0 支持
	{
		D3D12_FEATURE_DATA_SHADER_MODEL data = { .HighestShaderModel = D3D_SHADER_MODEL_6_0 };
		if (SUCCEEDED(_device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &data, sizeof(data)))) {
			_isSM6Supported = data.HighestShaderModel == D3D_SHADER_MODEL_6_0;
		}
	}

	if (!_InitializeDeviceResources(maxInFlightFrameCount, priority, commandListType, disableFrameFenceTracking)) {
		Logger::Get().Error("_InitializeDeviceResources 失败");
		return false;
	}

	if (!_descriptorHeap->Initialize(_device.get())) {
		Logger::Get().Error("DescriptorHeap::Initialize 失败");
		return false;
	}

	return true;
}

void GraphicsContext::CopyDevice(const GraphicsContext& other) {
	_descriptorHeap = other._descriptorHeap;
	_device = other._device;
	_rootSignatureVersion = other._rootSignatureVersion;
}

bool GraphicsContext::InitializeAfterCopyDevice(
	uint32_t maxInFlightFrameCount,
	D3D12_COMMAND_QUEUE_PRIORITY priority,
	D3D12_COMMAND_LIST_TYPE commandListType,
	bool disableFrameFenceTracking
) noexcept {
	HRESULT hr = _CreateDXGIFactory();
	if (FAILED(hr)) {
		Logger::Get().ComError("_CreateDXGIFactory 失败", hr);
		return false;
	}

	if (!_CreateAdapterFromDevice()) {
		Logger::Get().ComError("_CreateDXGIFactory 失败", hr);
		return false;
	}
	
	if (!_InitializeDeviceResources(maxInFlightFrameCount, priority, commandListType, disableFrameFenceTracking)) {
		Logger::Get().Error("_InitializeDeviceResources 失败");
		return false;
	}

	return true;
}

IDXGIFactory7* GraphicsContext::GetDXGIFactoryForEnumingAdapters() noexcept {
	if (!_dxgiFactory->IsCurrent()) {
		HRESULT hr = _CreateDXGIFactory();
		if (FAILED(hr)) {
			Logger::Get().ComError("_CreateDXGIFactory 失败", hr);
			return nullptr;
		}
	}

	return _dxgiFactory.get();
}

HRESULT GraphicsContext::Signal(uint64_t& fenceValue) noexcept {
	fenceValue = ++_curFenceValue;
	return _commandQueue->Signal(_fence.get(), _curFenceValue);
}

HRESULT GraphicsContext::WaitForFenceValue(uint64_t fenceValue) noexcept {
	if (_fence->GetCompletedValue() >= fenceValue) {
		return S_OK;
	} else {
		return _fence->SetEventOnCompletion(fenceValue, nullptr);
	}
}

HRESULT GraphicsContext::WaitForGpu() noexcept {
	HRESULT hr = _commandQueue->Signal(_fence.get(), ++_curFenceValue);
	if (FAILED(hr)) {
		Logger::Get().ComError("ID3D12CommandQueue::Signal 失败", hr);
		return hr;
	}

	return WaitForFenceValue(_curFenceValue);
}

HRESULT GraphicsContext::WaitForCommandQueue(ID3D12CommandQueue* commandQueue) noexcept {
	HRESULT hr = commandQueue->Signal(_fence.get(), ++_curFenceValue);
	if (FAILED(hr)) {
		Logger::Get().ComError("ID3D12CommandQueue::Signal 失败", hr);
		return hr;
	}

	hr = _commandQueue->Wait(_fence.get(), _curFenceValue);
	if (FAILED(hr)) {
		Logger::Get().ComError("ID3D12CommandQueue::Wait 失败", hr);
		return hr;
	}

	return S_OK;
}

HRESULT GraphicsContext::BeginFrame(uint32_t& curFrameIndex, ID3D12PipelineState* initialState) noexcept {
	if (!_frameFenceValues.empty()) {
		HRESULT hr = WaitForFenceValue(_frameFenceValues[_curFrameIndex]);
		if (FAILED(hr)) {
			Logger::Get().ComError("WaitForFenceValue 失败", hr);
			return hr;
		}
	}

	HRESULT hr = _commandAllocators[_curFrameIndex]->Reset();
	if (FAILED(hr)) {
		Logger::Get().ComError("ID3D12CommandAllocator::Reset 失败", hr);
		return hr;
	}

	hr = _commandList->Reset(_commandAllocators[_curFrameIndex].get(), initialState);
	if (FAILED(hr)) {
		Logger::Get().ComError("ID3D12GraphicsCommandList::Reset 失败", hr);
		return hr;
	}

	curFrameIndex = _curFrameIndex;
	return S_OK;
}

HRESULT GraphicsContext::EndFrame() noexcept {
	if (!_frameFenceValues.empty()) {
		HRESULT hr = Signal(_frameFenceValues[_curFrameIndex]);
		if (FAILED(hr)) {
			Logger::Get().ComError("Signal 失败", hr);
			return hr;
		}
	}

	_curFrameIndex = (_curFrameIndex + 1) % (uint32_t)_commandAllocators.size();
	return S_OK;
}

HRESULT GraphicsContext::_CreateDXGIFactory() noexcept {
	UINT flags = 0;
#ifdef _DEBUG
	flags |= DXGI_CREATE_FACTORY_DEBUG;
#endif
	HRESULT hr = CreateDXGIFactory2(flags, IID_PPV_ARGS(&_dxgiFactory));
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateDXGIFactory2 失败", hr);
	}
	return hr;
}

bool GraphicsContext::_InitializeDeviceResources(
	uint32_t maxInFlightFrameCount,
	D3D12_COMMAND_QUEUE_PRIORITY priority,
	D3D12_COMMAND_LIST_TYPE commandListType,
	bool disableFrameFenceTracking
) noexcept {
	{
		D3D12_COMMAND_QUEUE_DESC queueDesc = {
			.Type = commandListType,
			.Priority = priority,
			.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE
		};
		HRESULT hr = _device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&_commandQueue));
		if (FAILED(hr)) {
			Logger::Get().ComError("CreateCommandQueue 失败", hr);
			return false;
		}
	}

	HRESULT hr = _device->CreateCommandList1(0, commandListType,
		D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(&_commandList));
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateCommandList1 失败", hr);
		return false;
	}

	_commandAllocators.resize(maxInFlightFrameCount);
	for (winrt::com_ptr<ID3D12CommandAllocator>& commandAllocator : _commandAllocators) {
		hr = _device->CreateCommandAllocator(commandListType, IID_PPV_ARGS(&commandAllocator));
		if (FAILED(hr)) {
			Logger::Get().ComError("CreateCommandAllocator 失败", hr);
			return false;
		}
	}

	hr = _device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_fence));
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateFence 失败", hr);
		return false;
	}

	// 如果已在外部同步则无需追踪每帧的栅栏值
	if (!disableFrameFenceTracking) {
		_frameFenceValues.resize(maxInFlightFrameCount);
	}

	return true;
}

bool GraphicsContext::_CreateAdapterAndDevice(const GraphicsCardId& graphicsCardId) noexcept {
	winrt::com_ptr<IDXGIAdapter1> adapter;

#ifdef MP_DEBUG_INFO
	if (!DEBUG_INFO.useWarp) {
#endif
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
#ifdef MP_DEBUG_INFO
	}
#endif

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

bool GraphicsContext::_TryCreateD3DDevice(const winrt::com_ptr<IDXGIAdapter1>& adapter, const DXGI_ADAPTER_DESC1& adapterDesc) noexcept {
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
			Logger::Get().Info(fmt::format("已创建 D3D12 设备\n\t功能级别: {}", flStr));
		} else {
			Logger::Get().ComError("CheckFeatureSupport 失败", hr);
		}
	}

	_dxgiAdapter = adapter.try_as<IDXGIAdapter4>();
	if (!_dxgiAdapter) {
		Logger::Get().Error("获取 IDXGIAdapter4 失败");
		return false;
	}

	Logger::Get().Info(fmt::format("当前图形适配器: \n\tVendorId: {:#x}\n\tDeviceId: {:#x}\n\tDescription: {}",
		adapterDesc.VendorId, adapterDesc.DeviceId, StrHelper::UTF16ToUTF8(adapterDesc.Description)));

	return true;
}

bool GraphicsContext::_CreateAdapterFromDevice() noexcept {
	const LUID adapterLuid = _device->GetAdapterLuid();

	winrt::com_ptr<IDXGIAdapter1> adapter;
	for (UINT adapterIdx = 0;
		SUCCEEDED(_dxgiFactory->EnumAdapters1(adapterIdx, adapter.put()));
		++adapterIdx
	) {
		DXGI_ADAPTER_DESC1 desc;
		HRESULT hr = adapter->GetDesc1(&desc);
		if (FAILED(hr)) {
			continue;
		}

		if (desc.AdapterLuid != adapterLuid) {
			continue;
		}

		_dxgiAdapter = adapter.try_as<IDXGIAdapter4>();
		if (_dxgiAdapter) {
			return true;
		} else {
			Logger::Get().Error("获取 IDXGIAdapter4 失败");
			return false;
		}
	}

	return false;
}

}

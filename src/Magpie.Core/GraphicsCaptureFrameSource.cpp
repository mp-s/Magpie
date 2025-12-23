#include "pch.h"
#include "GraphicsCaptureFrameSource.h"
#include "ColorInfo.h"
#include "DebugInfo.h"
#include "DirectXHelper.h"
#include "GraphicsContext.h"
#include "Logger.h"
#include "ScalingWindow.h"
#include "Win32Helper.h"
#include "shaders/DuplicateFrameCS.h"
#include <dwmapi.h>
#include <Windows.Graphics.Capture.Interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

namespace winrt {
using namespace Windows::Graphics;
using namespace Windows::Graphics::Capture;
using namespace Windows::Graphics::DirectX;
using namespace Windows::Graphics::DirectX::Direct3D11;
}

namespace Magpie {

static constexpr uint32_t MAX_DIRTY_RECTS = 8;

GraphicsCaptureFrameSource::~GraphicsCaptureFrameSource() noexcept {
	if (_captureSession) {
		_StopCapture();
	}

	const HWND hwndSrc = ScalingWindow::Get().SrcTracker().Handle();

	// 还原源窗口圆角
	if (_isRoundCornerDisabled) {
		int value = DWMWCP_DEFAULT;
		HRESULT hr = DwmSetWindowAttribute(
			hwndSrc, DWMWA_WINDOW_CORNER_PREFERENCE, &value, sizeof(value));
		if (FAILED(hr)) {
			Logger::Get().ComError("取消禁用窗口圆角失败", hr);
		} else {
			Logger::Get().Info("已取消禁用窗口圆角");
		}
	}

	// 还原源窗口样式
	if (_isSrcStyleChanged) {
		const DWORD srcExStyle = GetWindowExStyle(hwndSrc);
		SetWindowLongPtr(hwndSrc, GWL_EXSTYLE, srcExStyle & ~WS_EX_APPWINDOW);
	}

	// 还原 Kirikiri 窗口
	if (_taskbarList) {
		_taskbarList->DeleteTab(hwndSrc);
		_taskbarList->AddTab(GetWindowOwner(hwndSrc));

		// 修正任务栏焦点窗口和 Alt+Tab 切换顺序
		if (GetForegroundWindow() == hwndSrc) {
			SetForegroundWindow(GetDesktopWindow());
			SetForegroundWindow(hwndSrc);
		}
	}
}

static winrt::com_ptr<IDXGIAdapter1> FindAdapterOfMonitor(IDXGIFactory7* dxgiFactory, HMONITOR hMon) noexcept {
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
			DXGI_OUTPUT_DESC desc;
			if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == hMon) {
				return adapter;
			}
		}
	}

	adapter = nullptr;
	return adapter;
}

static bool CalcWindowCapturedFrameBounds(HWND hWnd, RECT& rect) noexcept {
	// Graphics Capture 的捕获区域没有文档记录，这里的计算是我实验了多种窗口后得出的，
	// 高度依赖实现细节，未来可能会失效。
	// Win10 和 Win11 24H2 开始捕获区域为 extended frame bounds；Win11 24H2 前
	// DwmGetWindowAttribute 对最大化的窗口返回值和 Win10 不同，可能是 OS 的 bug，
	// 应进一步处理。
	HRESULT hr = DwmGetWindowAttribute(hWnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect));
	if (FAILED(hr)) {
		Logger::Get().ComError("DwmGetWindowAttribute 失败", hr);
		return false;
	}

	if (Win32Helper::GetWindowShowCmd(hWnd) != SW_SHOWMAXIMIZED ||
		Win32Helper::GetOSVersion().IsWin10() ||
		Win32Helper::GetOSVersion().Is24H2OrNewer()) {
		return true;
	}

	// 如果窗口禁用了非客户区域绘制则捕获区域为 extended frame bounds
	BOOL hasBorder = TRUE;
	hr = DwmGetWindowAttribute(hWnd, DWMWA_NCRENDERING_ENABLED, &hasBorder, sizeof(hasBorder));
	if (FAILED(hr)) {
		Logger::Get().ComError("DwmGetWindowAttribute 失败", hr);
		return false;
	}

	if (!hasBorder) {
		return true;
	}

	RECT clientRect;
	if (!Win32Helper::GetClientScreenRect(hWnd, clientRect)) {
		Logger::Get().Error("GetClientScreenRect 失败");
		return false;
	}

	// 有些窗口最大化后有部分客户区在屏幕外，如 UWP 和资源管理器，它们的捕获区域
	// 是整个客户区。否则捕获区域不会超出屏幕
	HMONITOR hMon = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
	MONITORINFO mi{ .cbSize = sizeof(mi) };
	if (!GetMonitorInfo(hMon, &mi)) {
		Logger::Get().Win32Error("GetMonitorInfo 失败");
		return false;
	}

	if (clientRect.top < mi.rcWork.top) {
		rect = clientRect;
	} else {
		Win32Helper::IntersectRect(rect, rect, mi.rcWork);
	}

	return true;
}

bool GraphicsCaptureFrameSource::Initialize(
	GraphicsContext& graphicsContext,
	const RECT& srcRect,
	HMONITOR hMonSrc,
	const ColorInfo& colorInfo
) noexcept {
	assert(hMonSrc);

	_graphicsContext = &graphicsContext;
	_isUsingScRGB = colorInfo.kind != winrt::AdvancedColorKind::StandardDynamicRange;

	if (!winrt::GraphicsCaptureSession::IsSupported()) {
		Logger::Get().Error("当前无法使用 Graphics Capture");
		return false;
	}

	{
		RECT frameBounds;
		if (!CalcWindowCapturedFrameBounds(ScalingWindow::Get().SrcHandle(), frameBounds)) {
			Logger::Get().Error("CalcWindowCapturedFrameBounds 失败");
			return false;
		}

		if (srcRect.left < frameBounds.left || srcRect.top < frameBounds.top) {
			Logger::Get().Error("裁剪边框错误");
			return false;
		}

		// 在源窗口存在 DPI 缩放时有时会有一像素的偏移（取决于窗口在屏幕上的位置）
		// 可能是 DwmGetWindowAttribute 的 bug
		_frameBox = {
			UINT(srcRect.left - frameBounds.left),
			UINT(srcRect.top - frameBounds.top),
			0,
			UINT(srcRect.right - frameBounds.left),
			UINT(srcRect.bottom - frameBounds.top),
			1
		};
	}

	_producerThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);

	ID3D12Device5* device = graphicsContext.GetDevice();

	{
		D3D12_COMMAND_QUEUE_DESC queueDesc = { .Type = D3D12_COMMAND_LIST_TYPE_COPY };
		HRESULT hr = device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&_copyCommandQueue));
		if (FAILED(hr)) {
			Logger::Get().ComError("CreateCommandQueue 失败", hr);
			return false;
		}
	}

	HRESULT hr = device->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_COPY,
		D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(&_copyCommandList));
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateCommandList1 失败", hr);
		return false;
	}

	const ScalingOptions& options = ScalingWindow::Get().Options();
	_slots.resize(options.maxProducerInFlightFrames);
	_curFrameIdx = options.maxProducerInFlightFrames - 1;

	{
		CD3DX12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE_DEFAULT);
		D3D12_HEAP_FLAGS heapFlag = graphicsContext.IsHeapFlagCreateNotZeroedSupported() ?
			D3D12_HEAP_FLAG_CREATE_NOT_ZEROED : D3D12_HEAP_FLAG_NONE;
		CD3DX12_RESOURCE_DESC texDesc = CD3DX12_RESOURCE_DESC::Tex2D(
			_isUsingScRGB ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_TYPELESS,
			UINT64(_frameBox.right - _frameBox.left),
			_frameBox.bottom - _frameBox.top,
			1, 1, 1, 0,
			D3D12_RESOURCE_FLAG_NONE
		);

		for (_FrameResourceSlot& slot : _slots) {
			hr = device->CreateCommandAllocator(
				D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&slot.commandAllocator));
			if (FAILED(hr)) {
				Logger::Get().ComError("CreateCommandAllocator 失败", hr);
				return false;
			}

			hr = device->CreateCommittedResource(&heapProperties, heapFlag,
				&texDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&slot.output));
			if (FAILED(hr)) {
				Logger::Get().ComError("CreateCommittedResource 失败", hr);
				return false;
			}
		}
	}

	if (!_CreateCaptureDevice(hMonSrc)) {
		Logger::Get().ComError("_CreateCaptureDevice 失败", hr);
		return false;
	}

	if (options.duplicateFrameDetectionMode != DuplicateFrameDetectionMode::Never) {
		ID3D12Device5* dfDevice = _bridgeDevice ? _bridgeDevice.get() : device;

		{
			// 需要快速获取结果，因此使用高优先级
			D3D12_COMMAND_QUEUE_DESC queueDesc = {
				.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE,
				.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH
			};
			hr = dfDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&_dfCommandQueue));
			if (FAILED(hr)) {
				Logger::Get().ComError("CreateCommandQueue 失败", hr);
				return false;
			}
		}

		hr = dfDevice->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_COMPUTE,
			D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(&_dfCommandList));
		if (FAILED(hr)) {
			Logger::Get().ComError("CreateCommandList1 失败", hr);
			return false;
		}

		hr = dfDevice->CreateCommandAllocator(
			D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&_dfCommandAllocator));
		if (FAILED(hr)) {
			Logger::Get().ComError("CreateCommandAllocator 失败", hr);
			return false;
		}

		{
			CD3DX12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE_DEFAULT);
			D3D12_HEAP_FLAGS heapFlag = graphicsContext.IsHeapFlagCreateNotZeroedSupported() ?
				D3D12_HEAP_FLAG_CREATE_NOT_ZEROED : D3D12_HEAP_FLAG_NONE;
			CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(
				MAX_DIRTY_RECTS * sizeof(uint32_t), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

			hr = dfDevice->CreateCommittedResource(&heapProperties, heapFlag,
				&desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&_dfResultBuffer));
			if (FAILED(hr)) {
				Logger::Get().ComError("CreateCommittedResource 失败", hr);
				return false;
			}

			heapProperties.Type = D3D12_HEAP_TYPE_READBACK;
			desc.Flags = D3D12_RESOURCE_FLAG_NONE;
			hr = dfDevice->CreateCommittedResource(&heapProperties, heapFlag,
				&desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&_dfResultReadbackBuffer));
			if (FAILED(hr)) {
				Logger::Get().ComError("CreateCommittedResource 失败", hr);
				return false;
			}
		}

		{
			// 容纳两个 SRV
			D3D12_DESCRIPTOR_HEAP_DESC desc = {
				.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
				.NumDescriptors = 2,
				.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
			};
			hr = dfDevice->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&_dfDescriptorHeap));
			if (FAILED(hr)) {
				Logger::Get().ComError("CreateDescriptorHeap 失败", hr);
				return false;
			}
		}

		_dfDescriptorSize = dfDevice->GetDescriptorHandleIncrementSize(
			D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		
		hr = dfDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_dfFence));
		if (FAILED(hr)) {
			Logger::Get().ComError("CreateFence 失败", hr);
			return false;
		}
		
		{
			winrt::com_ptr<ID3DBlob> signature;

			CD3DX12_DESCRIPTOR_RANGE1 srvRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0,
				D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE);

			D3D12_ROOT_PARAMETER1 rootParams[] = {
				{
					.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
					.Constants = {
						.Num32BitValues = 6
					}
				},
				{
					.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV,
					.Descriptor = {
						.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE
					}
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
				.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
				.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
				.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
				.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER,
				.ShaderRegister = 0
			};

			CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc(
				(UINT)std::size(rootParams), rootParams, 1, &samplerDesc);
			hr = D3DX12SerializeVersionedRootSignature(
				&rootSignatureDesc, graphicsContext.GetRootSignatureVersion(), signature.put(), nullptr);
			if (FAILED(hr)) {
				Logger::Get().ComError("D3DX12SerializeVersionedRootSignature 失败", hr);
				return false;
			}

			hr = dfDevice->CreateRootSignature(0, signature->GetBufferPointer(),
				signature->GetBufferSize(), IID_PPV_ARGS(&_dfRootSignature));
			if (FAILED(hr)) {
				Logger::Get().ComError("CreateRootSignature 失败", hr);
				return false;
			}
		}

		{
			D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {
				.pRootSignature = _dfRootSignature.get(),
				.CS = CD3DX12_SHADER_BYTECODE(DuplicateFrameCS, sizeof(DuplicateFrameCS))
			};
			hr = dfDevice->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&_dfPipelineState));
			if (FAILED(hr)) {
				Logger::Get().ComError("CreateComputePipelineState 失败", hr);
				return false;
			}
		}
	}

	if (!_InitializeCaptureItem()) {
		Logger::Get().Error("_InitializeCaptureItem 失败");
		return false;
	}

	return true;
}

bool GraphicsCaptureFrameSource::Start() noexcept {
	assert(!_captureSession);

	// 尽可能推迟禁用源窗口圆角
	if (!_isRoundCornerDisabled) {
		_DisableRoundCornerInWin11();
	}

	HRESULT hr = _StartCapture();
	if (FAILED(hr)) {
		Logger::Get().ComError("_StartCapture 失败", hr);
		return false;
	}

	return true;
}

static HRESULT GetFrameResourceFromCaptureFrame(
	const winrt::Direct3D11CaptureFrame& captureFrame,
	ID3D12Device5* device,
	winrt::com_ptr<ID3D12Resource>& frameResource
) noexcept {
	winrt::IDirect3DSurface d3dSurface = captureFrame.Surface();

	auto dxgiInterfaceAccess =
		d3dSurface.try_as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();

	winrt::com_ptr<ID3D11Texture2D> d3d11Texture;
	HRESULT hr = dxgiInterfaceAccess->GetInterface(IID_PPV_ARGS(&d3d11Texture));
	if (FAILED(hr)) {
		Logger::Get().ComError("IDirect3DDxgiInterfaceAccess::GetInterface 失败", hr);
		return hr;
	}

	auto dxgiResource = d3d11Texture.try_as<IDXGIResource1>();

	wil::unique_handle hSharedResource;
	hr = dxgiResource->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, hSharedResource.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("IDXGIResource1::CreateSharedHandle 失败", hr);
		return hr;
	}

	hr = device->OpenSharedHandle(hSharedResource.get(), IID_PPV_ARGS(&frameResource));
	if (FAILED(hr)) {
		Logger::Get().ComError("OpenSharedHandle 失败", hr);
		return hr;
	}

	return S_OK;
}

HRESULT GraphicsCaptureFrameSource::CheckForNewFrame(bool& isNewFrameAvailable) noexcept {
	{
		auto lk = _latestFrameLock.lock_shared();
		if (_latestFrame) {
			_newFrame = std::move(_latestFrame);
			_latestFrame = nullptr;
		}
	}

	if (!_newFrame) {
		isNewFrameAvailable = false;
		return S_OK;
	}

	ID3D12Device5* dfDevice = _bridgeDevice ? _bridgeDevice.get() : _graphicsContext->GetDevice();

	HRESULT hr = GetFrameResourceFromCaptureFrame(_newFrame, dfDevice, _newFrameResource);
	if (FAILED(hr)) {
		Logger::Get().ComError("GetFrameResourceFromCaptureFrame 失败", hr);
		return hr;
	}

	CD3DX12_SHADER_RESOURCE_VIEW_DESC desc = CD3DX12_SHADER_RESOURCE_VIEW_DESC::Tex2D(
		_isUsingScRGB ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM, 1);
	dfDevice->CreateShaderResourceView(_newFrameResource.get(), &desc, CD3DX12_CPU_DESCRIPTOR_HANDLE(
		_dfDescriptorHeap->GetCPUDescriptorHandleForHeapStart(), _dfCurDescriptorOffset));

	// 第一帧无需检查重复帧
	if (!_slots[_curFrameIdx].frameResource) {
		isNewFrameAvailable = true;
		return S_OK;
	}

	hr = _dfCommandAllocator->Reset();
	if (FAILED(hr)) {
		Logger::Get().ComError("ID3D12CommandAllocator::Reset 失败", hr);
		return hr;
	}

	hr = _dfCommandList->Reset(_dfCommandAllocator.get(), _dfPipelineState.get());
	if (FAILED(hr)) {
		Logger::Get().ComError("ID3D12GraphicsCommandList::Reset 失败", hr);
		return hr;
	}

	{
		ID3D12DescriptorHeap* t = _dfDescriptorHeap.get();
		_dfCommandList->SetDescriptorHeaps(1, &t);
	}

	_dfCommandList->SetComputeRootSignature(_dfRootSignature.get());

	{
#ifdef _DEBUG
		D3D12_RESOURCE_DESC texDesc = _newFrameResource->GetDesc();
		assert(texDesc.Width == _frameBox.right && texDesc.Height == _frameBox.bottom);
#endif
		DirectXHelper::Constant32 constants[] = {
			{.floatVal = 1.0f / _frameBox.right},
			{.floatVal = 1.0f / _frameBox.bottom},
			{.uintVal = _frameBox.left},
			{.uintVal = _frameBox.top},
			{.uintVal = 0},
			{.uintVal = ++_dfResultBufferTargetValue}
		};
		_dfCommandList->SetComputeRoot32BitConstants(0, (UINT)std::size(constants), constants, 0);
	}

	_dfCommandList->SetComputeRootUnorderedAccessView(1, _dfResultBuffer->GetGPUVirtualAddress());

	_dfCommandList->SetComputeRootDescriptorTable(
		2, _dfDescriptorHeap->GetGPUDescriptorHandleForHeapStart());

	constexpr uint32_t BLOCK_SIZE = 16;
	_dfCommandList->Dispatch(
		(_frameBox.right + BLOCK_SIZE - 1) / BLOCK_SIZE,
		(_frameBox.bottom + BLOCK_SIZE - 1) / BLOCK_SIZE,
		1
	);
	
	{
		CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
			_dfResultBuffer.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE, 0);
		_dfCommandList->ResourceBarrier(1, &barrier);
	}

	// TODO: 只复制需要的
	_dfCommandList->CopyBufferRegion(_dfResultReadbackBuffer.get(), 0, _dfResultBuffer.get(), 0, sizeof(uint32_t));

	hr = _dfCommandList->Close();
	if (FAILED(hr)) {
		Logger::Get().ComError("ID3D12GraphicsCommandList::Close 失败", hr);
		return hr;
	}

	{
		ID3D12CommandList* t = _dfCommandList.get();
		_dfCommandQueue->ExecuteCommandLists(1, &t);
	}

	hr = _dfCommandQueue->Signal(_dfFence.get(), ++_dfFenceValue);
	if (FAILED(hr)) {
		Logger::Get().ComError("ID3D12CommandQueue::Signal 失败", hr);
		return hr;
	}

	hr = _dfFence->SetEventOnCompletion(_dfFenceValue, NULL);
	if (FAILED(hr)) {
		Logger::Get().ComError("ID3D12Fence::SetEventOnCompletion 失败", hr);
		return hr;
	}

	// 读取结果
	{
		CD3DX12_RANGE range(0, sizeof(uint32_t));

		void* pData;
		hr = _dfResultReadbackBuffer->Map(0, nullptr, &pData);
		if (FAILED(hr)) {
			Logger::Get().ComError("ID3D12Resource::Map 失败", hr);
			return hr;
		}

		uint32_t* results = (uint32_t*)pData;
		isNewFrameAvailable = results[0] == _dfResultBufferTargetValue;

		range = {};
		_dfResultReadbackBuffer->Unmap(0, &range);
	}

	return S_OK;
}

HRESULT GraphicsCaptureFrameSource::Update(uint32_t& outputIdx) noexcept {
	if (!_newFrame) {
		// 没有新帧
		outputIdx = _curFrameIdx;
		return S_OK;
	}

	_curFrameIdx = (_curFrameIdx + 1) % (uint32_t)_slots.size();
	_FrameResourceSlot& curSlot = _slots[_curFrameIdx];

	curSlot.captureFrame = std::move(_newFrame);
	curSlot.frameResource = std::move(_newFrameResource);
	_newFrame = nullptr;
	_newFrameResource = nullptr;

	if (_dfCurDescriptorOffset == 0) {
		_dfCurDescriptorOffset = _dfDescriptorSize;
	} else {
		_dfCurDescriptorOffset = 0;
	}

#ifdef MP_DEBUG_INFO
	{
		auto lk = DEBUG_INFO.lock.lock_exclusive();

		if (DEBUG_INFO.dtmFrameNumer == 0) {
			DEBUG_INFO.dtmDwmQPC = curSlot.captureFrame.SystemRelativeTime().count();
			DEBUG_INFO.dtmFrameNumer = DEBUG_INFO.producerFrameNumber;
		}

		if (DEBUG_INFO.ctpCapturedFrame && DEBUG_INFO.ctpFrameNumer == 0) {
			if (winrt::get_abi(curSlot.captureFrame) == DEBUG_INFO.ctpCapturedFrame) {
				DEBUG_INFO.ctpFrameNumer = DEBUG_INFO.producerFrameNumber;
			} else {
				// 追踪的捕获帧被错过，需要重新测量
				DEBUG_INFO.ctpCapturedFrame = nullptr;
			}
		}
	}
#endif

	// 同适配器数据路径: frameResource -> output
	// 跨适配器数据路径: frameResource -> bridgeResource|sharedResource -> output

	HRESULT hr = curSlot.commandAllocator->Reset();
	if (FAILED(hr)) {
		return hr;
	}

	hr = _copyCommandList->Reset(curSlot.commandAllocator.get(), nullptr);
	if (FAILED(hr)) {
		return hr;
	}

	if (_bridgeDevice) {
		_FrameCrossAdapterResourceSlot& curCASlot = _crossAdapterSlots[_curFrameIdx];

		hr = curCASlot.commandAllocator->Reset();
		if (FAILED(hr)) {
			return hr;
		}

		hr = _bridgeCopyCommandList->Reset(curCASlot.commandAllocator.get(), nullptr);
		if (FAILED(hr)) {
			return hr;
		}

		{
			CD3DX12_TEXTURE_COPY_LOCATION src(curSlot.frameResource.get(), 0);
			CD3DX12_TEXTURE_COPY_LOCATION dest(curCASlot.bridgeResource.get(), 0);
			_bridgeCopyCommandList->CopyTextureRegion(&dest, 0, 0, 0, &src, &_frameBox);
		}

		hr = _bridgeCopyCommandList->Close();
		if (FAILED(hr)) {
			Logger::Get().ComError("ID3D12GraphicsCommandList::Close 失败", hr);
			return hr;
		}

		{
			ID3D12CommandList* t = _bridgeCopyCommandList.get();
			_bridgeCopyCommandQueue->ExecuteCommandLists(1, &t);
		}

		hr = _bridgeCopyCommandQueue->Signal(_bridgeFence.get(), ++_curCrossAdapterFenceValue);
		if (FAILED(hr)) {
			Logger::Get().ComError("ID3D12CommandQueue::Signal 失败", hr);
			return hr;
		}

		hr = _copyCommandQueue->Wait(_sharedFence.get(), _curCrossAdapterFenceValue);
		if (FAILED(hr)) {
			Logger::Get().ComError("ID3D12CommandQueue::Wait 失败", hr);
			return hr;
		}

		_copyCommandList->CopyResource(curSlot.output.get(), curCASlot.sharedResource.get());
	} else {
		CD3DX12_TEXTURE_COPY_LOCATION src(curSlot.frameResource.get(), 0);
		CD3DX12_TEXTURE_COPY_LOCATION dest(curSlot.output.get(), 0);
		_copyCommandList->CopyTextureRegion(&dest, 0, 0, 0, &src, &_frameBox);
	}
	
	hr = _copyCommandList->Close();
	if (FAILED(hr)) {
		Logger::Get().ComError("ID3D12GraphicsCommandList::Close 失败", hr);
		return hr;
	}

	{
		ID3D12CommandList* t = _copyCommandList.get();
		_copyCommandQueue->ExecuteCommandLists(1, &t);
	}

	hr = _graphicsContext->WaitForCommandQueue(_copyCommandQueue.get());
	if (FAILED(hr)) {
		Logger::Get().ComError("GraphicsContext::WaitForCommandQueue 失败", hr);
		return hr;
	}

	outputIdx = _curFrameIdx;
	return S_OK;
}

// 显示光标时需要重启捕获，否则光标可能不会立刻显示
HRESULT GraphicsCaptureFrameSource::OnCursorVisibilityChanged(bool isVisible, bool onDestory) noexcept {
	if (!isVisible) {
		return S_OK;
	}

	HRESULT hr = _graphicsContext->WaitForGpu();
	if (FAILED(hr)) {
		Logger::Get().ComError("GraphicsContext::WaitForGpu 失败", hr);
		return hr;
	}

	_StopCapture();

	if (onDestory) {
		// FIXME: 这里尝试修复拖动窗口时光标不显示的问题，但有些环境下不起作用
		SystemParametersInfo(SPI_SETCURSORS, 0, nullptr, 0);
	} else {
		hr = _StartCapture();
		if (FAILED(hr)) {
			Logger::Get().ComError("_StartCapture 失败", hr);
			return hr;
		}
	}

	return S_OK;
}

static bool IsDebugLayersAvailable() noexcept {
#ifdef _DEBUG
	static bool result = SUCCEEDED(D3D11CreateDevice(
		nullptr,
		D3D_DRIVER_TYPE_NULL,       // There is no need to create a real hardware device.
		nullptr,
		D3D11_CREATE_DEVICE_DEBUG,  // Check for the SDK layers.
		nullptr,                    // Any feature level will do.
		0,
		D3D11_SDK_VERSION,
		nullptr,                    // No need to keep the D3D device reference.
		nullptr,                    // No need to know the feature level.
		nullptr                     // No need to keep the D3D device context reference.
	));
	return result;
#else
	// Relaese 配置不使用调试层
	return false;
#endif
}

bool GraphicsCaptureFrameSource::_CreateCaptureDevice(HMONITOR hMonSrc) noexcept {
	// 查找源窗口所在屏幕连接的适配器
	winrt::com_ptr<IDXGIAdapter1> srcMonAdapter =
		FindAdapterOfMonitor(_graphicsContext->GetDXGIFactoryForEnumingAdapters(), hMonSrc);
	if (srcMonAdapter) {
		DXGI_ADAPTER_DESC desc;
		HRESULT hr = srcMonAdapter->GetDesc(&desc);
		if (SUCCEEDED(hr)) {
			if (desc.AdapterLuid != _graphicsContext->GetDevice()->GetAdapterLuid()) {
				// 跨适配器捕获
				if (!_CreateBridgeDeviceResources(srcMonAdapter.get())) {
					// 失败则使用渲染设备捕获，交给 WGC 中转
					srcMonAdapter = nullptr;

					// 清理跨适配器资源
					_bridgeDevice = nullptr;
					_bridgeCopyCommandQueue = nullptr;
					_bridgeCopyCommandList = nullptr;
					_sharedHeap = nullptr;
					_bridgeHeap = nullptr;
					_sharedFence = nullptr;
					_bridgeFence = nullptr;
					_crossAdapterSlots.clear();
				}
			}
		} else {
			Logger::Get().ComError("IDXGIAdapter1::GetDesc 失败", hr);
		}
	}

	D3D_FEATURE_LEVEL featureLevels[] = {
		D3D_FEATURE_LEVEL_11_1,
		D3D_FEATURE_LEVEL_11_0
	};
	const UINT nFeatureLevels = ARRAYSIZE(featureLevels);

	UINT createDeviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
	if (IsDebugLayersAvailable()) {
		createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
	}

	winrt::com_ptr<ID3D11Device> d3dDevice;
	winrt::com_ptr<ID3D11DeviceContext> d3dDC;
	D3D_FEATURE_LEVEL featureLevel;
	HRESULT hr = D3D11CreateDevice(
		srcMonAdapter ? srcMonAdapter.get() : _graphicsContext->GetDXGIAdapter(),
		D3D_DRIVER_TYPE_UNKNOWN,
		nullptr,
		createDeviceFlags,
		featureLevels,
		nFeatureLevels,
		D3D11_SDK_VERSION,
		d3dDevice.put(),
		&featureLevel,
		d3dDC.put()
	);

	if (FAILED(hr)) {
		Logger::Get().ComError("D3D11CreateDevice 失败", hr);
		return false;
	}

	std::string_view fl;
	switch (featureLevel) {
	case D3D_FEATURE_LEVEL_11_1:
		fl = "11.1";
		break;
	case D3D_FEATURE_LEVEL_11_0:
		fl = "11.0";
		break;
	default:
		fl = "未知";
		break;
	}
	Logger::Get().Info(fmt::format("已创建 D3D11 设备\n\t功能级别: {}", fl));

	_d3d11Device = d3dDevice.try_as<ID3D11Device5>();
	if (!_d3d11Device) {
		Logger::Get().Error("获取 ID3D11Device5 失败");
		return false;
	}

	_d3d11DC = d3dDC.try_as<ID3D11DeviceContext4>();
	if (!_d3d11DC) {
		Logger::Get().Error("获取 ID3D11DeviceContext4 失败");
		return false;
	}

	return true;
}

// 通过 D3D12 跨适配器共享机制共享捕获图像
bool GraphicsCaptureFrameSource::_CreateBridgeDeviceResources(IDXGIAdapter1* dxgiAdapter) noexcept {
	HRESULT hr = D3D12CreateDevice(dxgiAdapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&_bridgeDevice));
	if (FAILED(hr)) {
		Logger::Get().ComError("D3D12CreateDevice 失败", hr);
		return false;
	}

	Logger::Get().Info("已创建 D3D12 设备");

	// 不应使用集成显卡捕获，集成显卡没有高速的专用显存，捕获延迟很高
	{
		D3D12_FEATURE_DATA_ARCHITECTURE1 value{};
		hr = _bridgeDevice->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE1, &value, sizeof(value));
		if (FAILED(hr)) {
			Logger::Get().ComError("CheckFeatureSupport 失败", hr);
			return false;
		}

		if (value.UMA) {
			Logger::Get().Info("不使用集成显卡捕获");
			return false;
		}
	}

	ID3D12Device5* device = _graphicsContext->GetDevice();
	const uint32_t frameCount = ScalingWindow::Get().Options().maxProducerInFlightFrames;

	_crossAdapterSlots.resize(frameCount);

	{
		D3D12_COMMAND_QUEUE_DESC queueDesc = { .Type = D3D12_COMMAND_LIST_TYPE_COPY };
		hr = _bridgeDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&_bridgeCopyCommandQueue));
		if (FAILED(hr)) {
			Logger::Get().ComError("CreateCommandQueue 失败", hr);
			return false;
		}
	}

	hr = _bridgeDevice->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_COPY,
		D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(&_bridgeCopyCommandList));
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateCommandList1 失败", hr);
		return false;
	}

	CD3DX12_RESOURCE_DESC textureDesc = CD3DX12_RESOURCE_DESC::Tex2D(
		_isUsingScRGB ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM,
		UINT64(_frameBox.right - _frameBox.left),
		_frameBox.bottom - _frameBox.top,
		1, 1, 1, 0,
		D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER,
		D3D12_TEXTURE_LAYOUT_ROW_MAJOR
	);

	D3D12_RESOURCE_ALLOCATION_INFO textureInfo = _bridgeDevice->GetResourceAllocationInfo(0, 1, &textureDesc);

	// 创建跨适配器共享堆。应遵循“写入者创建”的原则，否则可能无法正确同步，Intel 集显作为
	// 捕获设备时存在这个问题。
	{
		const bool isCreateNotZeroedSupported = (bool)_bridgeDevice.try_as<ID3D12Device8>();
		CD3DX12_HEAP_DESC heapDesc(
			textureInfo.SizeInBytes * frameCount,
			D3D12_HEAP_TYPE_DEFAULT,
			0,
			D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER |
				(isCreateNotZeroedSupported ? D3D12_HEAP_FLAG_CREATE_NOT_ZEROED : D3D12_HEAP_FLAG_NONE)
		);
		hr = _bridgeDevice->CreateHeap(&heapDesc, IID_PPV_ARGS(&_bridgeHeap));
		if (FAILED(hr)) {
			Logger::Get().ComError("CreateHeap 失败", hr);
			return false;
		}

		wil::unique_handle hSharedHeap;
		hr = _bridgeDevice->CreateSharedHandle(
			_bridgeHeap.get(), nullptr, GENERIC_ALL, nullptr, hSharedHeap.put());
		if (FAILED(hr)) {
			Logger::Get().ComError("CreateSharedHandle 失败", hr);
			return false;
		}

		hr = device->OpenSharedHandle(hSharedHeap.get(), IID_PPV_ARGS(&_sharedHeap));
		if (FAILED(hr)) {
			Logger::Get().ComError("OpenSharedHandle 失败", hr);
			return false;
		}
	}
	
	for (uint32_t i = 0; i < frameCount; ++i) {
		_FrameCrossAdapterResourceSlot& curSlot = _crossAdapterSlots[i];

		hr = _bridgeDevice->CreateCommandAllocator(
			D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&curSlot.commandAllocator));
		if (FAILED(hr)) {
			Logger::Get().ComError("CreateCommandAllocator 失败", hr);
			return false;
		}

		hr = _bridgeDevice->CreatePlacedResource(
			_bridgeHeap.get(),
			textureInfo.SizeInBytes * i,
			&textureDesc,
			D3D12_RESOURCE_STATE_COMMON,
			nullptr,
			IID_PPV_ARGS(&curSlot.bridgeResource)
		);
		if (FAILED(hr)) {
			Logger::Get().ComError("CreatePlacedResource 失败", hr);
			return false;
		}

		hr = device->CreatePlacedResource(
			_sharedHeap.get(),
			textureInfo.SizeInBytes * i,
			&textureDesc,
			D3D12_RESOURCE_STATE_COMMON,
			nullptr,
			IID_PPV_ARGS(&curSlot.sharedResource)
		);
		if (FAILED(hr)) {
			Logger::Get().ComError("CreatePlacedResource 失败", hr);
			return false;
		}
	}

	// 创建跨适配器栅栏，遵循“写入者创建”的原则
	hr = _bridgeDevice->CreateFence(
		0,
		D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER,
		IID_PPV_ARGS(&_bridgeFence)
	);
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateFence 失败", hr);
		return false;
	}

	wil::unique_handle hSharedFence;
	hr = _bridgeDevice->CreateSharedHandle(
		_bridgeFence.get(), nullptr, GENERIC_ALL, nullptr, hSharedFence.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateSharedHandle 失败", hr);
		return false;
	}

	hr = device->OpenSharedHandle(hSharedFence.get(), IID_PPV_ARGS(&_sharedFence));
	if (FAILED(hr)) {
		Logger::Get().ComError("OpenSharedHandle 失败", hr);
		return false;
	}

	return true;
}

// 部分使用 Kirikiri 引擎的游戏有着这样的架构: 游戏窗口并非顶级窗口，而是被一个零尺寸
// 的窗口所有。此时 Alt+Tab 列表中的窗口和任务栏图标实际上是所有者窗口，这会导致 WGC
// 捕获失败。我们特殊处理这类窗口。
static bool IsKirikiriWindow(HWND hwndSrc) noexcept {
	const HWND hwndOwner = GetWindowOwner(hwndSrc);
	if (!hwndOwner) {
		return false;
	}

	RECT ownerRect;
	if (!GetWindowRect(hwndOwner, &ownerRect)) {
		Logger::Get().Win32Error("GetWindowRect 失败");
		return false;
	}

	// 所有者窗口尺寸为零，而且是顶级窗口
	return ownerRect.left == ownerRect.right && ownerRect.top == ownerRect.bottom &&
		!GetWindowOwner(hwndOwner);
}

bool GraphicsCaptureFrameSource::_InitializeCaptureItem() noexcept {
	winrt::com_ptr<IDXGIDevice> dxgiDevice;
	HRESULT hr = _d3d11Device->QueryInterface<IDXGIDevice>(dxgiDevice.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("获取 IDXGIDevice 失败", hr);
		return false;
	}

	hr = CreateDirect3D11DeviceFromDXGIDevice(
		dxgiDevice.get(), (IInspectable**)winrt::put_abi(_wrappedDevice));
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateDirect3D11DeviceFromDXGIDevice 失败", hr);
		return false;
	}

	winrt::com_ptr<IGraphicsCaptureItemInterop> interop =
		winrt::try_get_activation_factory<winrt::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
	if (!interop) {
		Logger::Get().Error("获取 IGraphicsCaptureItemInterop 失败");
		return false;
	}

	const HWND hwndSrc = ScalingWindow::Get().SrcHandle();

	const DWORD srcExStyle = GetWindowExStyle(hwndSrc);
	// WS_EX_APPWINDOW 样式使窗口始终在 Alt+Tab 列表中显示
	if (srcExStyle & WS_EX_APPWINDOW) {
		hr = interop->CreateForWindow(
			hwndSrc, winrt::guid_of<winrt::GraphicsCaptureItem>(), winrt::put_abi(_captureItem));
		if (FAILED(hr)) {
			Logger::Get().ComError("IGraphicsCaptureItemInterop::CreateForWindow 失败", hr);
			return false;
		}

		return true;
	}

	// 第一次尝试捕获。Kirikiri 窗口必定失败，无需尝试
	const bool isSrcKirikiri = IsKirikiriWindow(hwndSrc);
	if (isSrcKirikiri) {
		Logger::Get().Info("源窗口有零尺寸的所有者窗口");
	} else {
		hr = interop->CreateForWindow(
			hwndSrc, winrt::guid_of<winrt::GraphicsCaptureItem>(), winrt::put_abi(_captureItem));
		if (SUCCEEDED(hr)) {
			return true;
		} else {
			Logger::Get().ComError("IGraphicsCaptureItemInterop::CreateForWindow 失败", hr);
		}
	}

	// 添加 WS_EX_APPWINDOW 样式
	if (!SetWindowLongPtr(hwndSrc, GWL_EXSTYLE, srcExStyle | WS_EX_APPWINDOW)) {
		Logger::Get().Win32Error("SetWindowLongPtr 失败");
		return false;
	}

	Logger::Get().Info("已改变源窗口样式");
	_isSrcStyleChanged = true;

	// Kirikiri 窗口改变样式后所有者窗口和游戏窗口将同时出现在 Alt+Tab 列表和任务栏中。
	// 虽然所有窗口都会如此，但 Kirikiri 的特殊之处在于两个窗口的图标和标题相同，为了不
	// 引起困惑应隐藏所有者窗口的图标。
	if (isSrcKirikiri) {
		_taskbarList = winrt::try_create_instance<ITaskbarList>(CLSID_TaskbarList);
		if (_taskbarList) {
			hr = _taskbarList->HrInit();
			if (SUCCEEDED(hr)) {
				// 修正任务栏图标
				_taskbarList->DeleteTab(GetWindowOwner(hwndSrc));
				_taskbarList->AddTab(hwndSrc);

				// 修正 Alt+Tab 切换顺序
				if (GetForegroundWindow() == hwndSrc) {
					SetForegroundWindow(GetDesktopWindow());
					SetForegroundWindow(hwndSrc);
				}
			} else {
				Logger::Get().ComError("ITaskbarList::HrInit 失败", hr);
				_taskbarList = nullptr;
			}
		} else {
			Logger::Get().Error("创建 ITaskbarList 失败");
		}
	}

	// 再次尝试捕获
	hr = interop->CreateForWindow(
		hwndSrc, winrt::guid_of<winrt::GraphicsCaptureItem>(), winrt::put_abi(_captureItem));
	if (FAILED(hr)) {
		Logger::Get().ComError("IGraphicsCaptureItemInterop::CreateForWindow 失败", hr);

		if (_isSrcStyleChanged) {
			// 恢复源窗口样式
			SetWindowLongPtr(hwndSrc, GWL_EXSTYLE, srcExStyle);
			_isSrcStyleChanged = false;
		}

		return false;
	}

	return true;
}

void GraphicsCaptureFrameSource::_Direct3D11CaptureFramePool_FrameArrived(
	const winrt::Direct3D11CaptureFramePool& pool,
	const winrt::IInspectable&
) {
	winrt::Direct3D11CaptureFrame frame = pool.TryGetNextFrame();
	if (!frame) {
		return;
	}

	// 取最新帧
	while (true) {
		if (winrt::Direct3D11CaptureFrame nextFrame = pool.TryGetNextFrame()) {
			frame = std::move(nextFrame);
		} else {
			break;
		}
	}

	{
		auto lk = _latestFrameLock.lock_exclusive();
		_latestFrame = std::move(frame);

#ifdef MP_DEBUG_INFO
		{
			auto debugLock = DEBUG_INFO.lock.lock_exclusive();

			if (!DEBUG_INFO.ctpCapturedFrame) {
				LARGE_INTEGER counter;
				QueryPerformanceCounter(&counter);
				DEBUG_INFO.ctpCaptureQPC = counter.QuadPart;

				DEBUG_INFO.ctpCapturedFrame = winrt::get_abi(_latestFrame);
			}
		}
#endif
	}

	// 唤起生产者线程
	PostThreadMessage(_producerThreadId.load(std::memory_order_relaxed), WM_NULL, 0, 0);
}

void GraphicsCaptureFrameSource::_DisableRoundCornerInWin11() noexcept {
	if (Win32Helper::GetOSVersion().IsWin10()) {
		return;
	}

	const HWND hwndSrc = ScalingWindow::Get().SrcHandle();

	int value = DWMWCP_DONOTROUND;
	HRESULT hr = DwmSetWindowAttribute(
		hwndSrc, DWMWA_WINDOW_CORNER_PREFERENCE, &value, sizeof(value));
	if (FAILED(hr)) {
		Logger::Get().ComError("禁用窗口圆角失败", hr);
		return;
	}

	_isRoundCornerDisabled = true;
}

HRESULT GraphicsCaptureFrameSource::_StartCapture() noexcept {
	assert(!_captureFramePool && !_captureSession);

	try {
		// 创建帧缓冲池。帧的尺寸为包含源窗口的最小尺寸
		_captureFramePool = winrt::Direct3D11CaptureFramePool::CreateFreeThreaded(
			_wrappedDevice,
			_isUsingScRGB ? winrt::DirectXPixelFormat::R16G16B16A16Float : winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized,
			ScalingWindow::Get().Options().maxProducerInFlightFrames + 3,
			{ (int)_frameBox.right, (int)_frameBox.bottom }
		);

		_captureFramePool.FrameArrived(
			{ this, &GraphicsCaptureFrameSource::_Direct3D11CaptureFramePool_FrameArrived });

		_captureSession = _captureFramePool.CreateCaptureSession(_captureItem);

		// 禁止捕获光标。从 Win10 v2004 开始支持
		if (winrt::ApiInformation::IsPropertyPresent(
			winrt::name_of<winrt::GraphicsCaptureSession>(),
			L"IsCursorCaptureEnabled"
		)) {
			_captureSession.IsCursorCaptureEnabled(false);
		}

		// 不显示黄色边框，Win32 应用中无需请求权限。从 Win11 开始支持
		if (winrt::ApiInformation::IsPropertyPresent(
			winrt::name_of<winrt::GraphicsCaptureSession>(),
			L"IsBorderRequired"
		)) {
			_captureSession.IsBorderRequired(false);
		}

		// Win11 24H2 中必须设置 MinUpdateInterval 才能使捕获帧率超过 60FPS
		if (winrt::ApiInformation::IsPropertyPresent(
			winrt::name_of<winrt::GraphicsCaptureSession>(),
			L"MinUpdateInterval"
		)) {
			_captureSession.MinUpdateInterval(1ms);
		}

		_captureSession.StartCapture();
	} catch (const winrt::hresult_error& e) {
		Logger::Get().ComInfo(StrHelper::Concat("启动捕获失败: ", StrHelper::UTF16ToUTF8(e.message())), e.code());
		return e.code();
	}

	return S_OK;
}

// 调用前应等待 GPU 
void GraphicsCaptureFrameSource::_StopCapture() noexcept {
	assert(_captureFramePool && _captureSession);

	_captureSession.Close();
	_captureSession = nullptr;

	_captureFramePool.Close();
	_captureFramePool = nullptr;

	// 可以直接释放，因为不会再触发 FrameArrived
	{
		auto lk = _latestFrameLock.lock_exclusive();
		_latestFrame = nullptr;
	}
	
	for (_FrameResourceSlot& slot : _slots) {
		// output 将继续使用，直到重启捕获
		slot.captureFrame = nullptr;
		slot.frameResource = nullptr;
	}
}

}

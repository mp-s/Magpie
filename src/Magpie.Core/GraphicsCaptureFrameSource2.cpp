#include "pch.h"
#include "GraphicsCaptureFrameSource2.h"
#include "GraphicsContext.h"
#include "Logger.h"
#include "Win32Helper.h"
#include "ScalingWindow.h"
#include "ColorInfo.h"
#include "DirectXHelper.h"
#include "DebugInfo.h"
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

GraphicsCaptureFrameSource2::~GraphicsCaptureFrameSource2() noexcept {
	_StopCapture();
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

bool GraphicsCaptureFrameSource2::Initialize(
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

	const uint32_t maxInFlightFrames = ScalingWindow::Get().Options().maxProducerInFlightFrames;
	_slots.resize(maxInFlightFrames);
	_curFrameIdx = maxInFlightFrames - 1;

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

	winrt::com_ptr<IDXGIDevice> dxgiDevice;
	hr = _d3d11Device->QueryInterface<IDXGIDevice>(dxgiDevice.put());
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

	hr = interop->CreateForWindow(
		ScalingWindow::Get().SrcHandle(),
		winrt::guid_of<winrt::GraphicsCaptureItem>(),
		winrt::put_abi(_captureItem)
	);
	if (FAILED(hr)) {
		Logger::Get().ComError("IGraphicsCaptureItemInterop::CreateForWindow 失败", hr);
		return false;
	}

	return true;
}

bool GraphicsCaptureFrameSource2::Start() noexcept {
	if (_captureSession) {
		return true;
	}

	try {
		// 创建帧缓冲池。帧的尺寸和 _captureItem.Size() 不同
		_captureFramePool = winrt::Direct3D11CaptureFramePool::CreateFreeThreaded(
			_wrappedDevice,
			_isUsingScRGB ? winrt::DirectXPixelFormat::R16G16B16A16Float : winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized,
			ScalingWindow::Get().Options().maxProducerInFlightFrames + 3,
			{ (int)_frameBox.right, (int)_frameBox.bottom } // 帧的尺寸为包含源窗口的最小尺寸
		);

		_captureFramePool.FrameArrived(
			{ this, &GraphicsCaptureFrameSource2::_Direct3D11CaptureFramePool_FrameArrived });

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
		Logger::Get().Info(StrHelper::Concat("Graphics Capture 失败: ", StrHelper::UTF16ToUTF8(e.message())));
		return false;
	}

	return true;
}

FrameSourceState GraphicsCaptureFrameSource2::GetState() noexcept {
	{
		auto lk = _latestFrameLock.lock_shared();
		if (_latestFrame) {
			return FrameSourceState::NewFrameAvailable;
		}
	}
	
	return _slots[_curFrameIdx].captureFrame ?
		FrameSourceState::Waiting : FrameSourceState::WaitingForFirstFrame;
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

HRESULT GraphicsCaptureFrameSource2::Update(uint32_t& outputIdx) noexcept {
	{
		// 不要在持有 _latestFrameLock 时释放 Direct3D11CaptureFrame。WGC 内部使用 Critical Section
		// 同步，如果此时 _Direct3D11CaptureFramePool_FrameArrived 正在执行会死锁。
		winrt::Direct3D11CaptureFrame latestFrame{ nullptr };
		{
			auto lk = _latestFrameLock.lock_exclusive();
			latestFrame = std::move(_latestFrame);
			_latestFrame = nullptr;
		}

		if (!latestFrame) {
			// 没有新帧
			outputIdx = _curFrameIdx;
			return S_OK;
		}

		_curFrameIdx = (_curFrameIdx + 1) % (uint32_t)_slots.size();

		_FrameResourceSlot& curSlot = _slots[_curFrameIdx];
		curSlot.frameResource = nullptr;
		curSlot.captureFrame = std::move(latestFrame);
	}

	_FrameResourceSlot& curSlot = _slots[_curFrameIdx];

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

	HRESULT hr = GetFrameResourceFromCaptureFrame(
		curSlot.captureFrame,
		_bridgeDevice ? _bridgeDevice.get() : _graphicsContext->GetDevice(),
		curSlot.frameResource
	);
	if (FAILED(hr)) {
		Logger::Get().ComError("GetFrameResourceFromCaptureFrame 失败", hr);
		return hr;
	}

	// 同适配器数据路径: frameResource -> output
	// 跨适配器数据路径: frameResource -> bridgeResource|sharedResource -> output

	hr = curSlot.commandAllocator->Reset();
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

		// D3D11 共享纹理有 D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS 标志，因此无需屏障
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
		// D3D11 共享纹理有 D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS 标志，因此无需屏障
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

bool GraphicsCaptureFrameSource2::_CreateCaptureDevice(HMONITOR hMonSrc) noexcept {
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
bool GraphicsCaptureFrameSource2::_CreateBridgeDeviceResources(IDXGIAdapter1* dxgiAdapter) noexcept {
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

void GraphicsCaptureFrameSource2::_Direct3D11CaptureFramePool_FrameArrived(
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

void GraphicsCaptureFrameSource2::_StopCapture() noexcept {
	if (_captureSession) {
		_captureSession.Close();
		_captureSession = nullptr;
	}
	if (_captureFramePool) {
		_captureFramePool.Close();
		_captureFramePool = nullptr;
	}
}

}

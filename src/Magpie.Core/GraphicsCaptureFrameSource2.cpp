#include "pch.h"
#include "GraphicsCaptureFrameSource2.h"
#include "Logger.h"
#include "Win32Helper.h"
#include "ScalingWindow.h"
#include <dwmapi.h>
#include <windows.graphics.directx.direct3d11.interop.h>

namespace winrt {
using namespace Windows::Graphics;
using namespace Windows::Graphics::Capture;
using namespace Windows::Graphics::DirectX;
using namespace Windows::Graphics::DirectX::Direct3D11;
}

namespace Magpie {

GraphicsCaptureFrameSource2::~GraphicsCaptureFrameSource2() {
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
	const auto& srcTracker = ScalingWindow::Get().SrcTracker();
	rect = srcTracker.WindowFrameRect();

	if (!srcTracker.IsZoomed() ||
		Win32Helper::GetOSVersion().IsWin10() ||
		Win32Helper::GetOSVersion().Is24H2OrNewer()) {
		return true;
	}

	// 如果窗口禁用了非客户区域绘制则捕获区域为 extended frame bounds
	BOOL hasBorder = TRUE;
	HRESULT hr = DwmGetWindowAttribute(hWnd, DWMWA_NCRENDERING_ENABLED, &hasBorder, sizeof(hasBorder));
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
	ID3D12Device5* device,
	IDXGIFactory7* dxgiFactory,
	IDXGIAdapter4* dxgiAdapter,
	HMONITOR hMonSrc,
	bool useScRGB
) noexcept {
	assert(hMonSrc);

	_renderingDevice = device;
	_isUsingScRGB = useScRGB;

	if (!winrt::GraphicsCaptureSession::IsSupported()) {
		Logger::Get().Error("当前无法使用 Graphics Capture");
		return false;
	}

	{
		const SrcTracker& srcTracker = ScalingWindow::Get().SrcTracker();
		const HWND hwndSrc = srcTracker.Handle();
		const RECT& srcRect = srcTracker.SrcRect();

		RECT frameBounds;
		if (!CalcWindowCapturedFrameBounds(hwndSrc, frameBounds)) {
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

	// 查找源窗口所在屏幕连接的适配器
	winrt::com_ptr<IDXGIAdapter1> srcMonAdapter = FindAdapterOfMonitor(dxgiFactory, hMonSrc);

	if (srcMonAdapter) {
		DXGI_ADAPTER_DESC desc;
		HRESULT hr = srcMonAdapter->GetDesc(&desc);
		if (FAILED(hr)) {
			Logger::Get().ComError("IDXGIAdapter1::GetDesc 失败", hr);
			return false;
		}

		const LUID renderAdapterLUID = device->GetAdapterLuid();

		if (desc.AdapterLuid.HighPart != renderAdapterLUID.HighPart ||
			desc.AdapterLuid.LowPart != renderAdapterLUID.LowPart)
		{
			// 通过 D3D12 跨适配器共享机制共享捕获图像
			if (!_CreateBridgeDeviceResources(dxgiAdapter)) {
				// 失败则使用渲染设备捕获，WGC 内部使用内存中转
				srcMonAdapter.copy_from(dxgiAdapter);
			}
		}
		
	} else {
		// 找不到源窗口所在屏幕的适配器则使用渲染设备
		srcMonAdapter.copy_from(dxgiAdapter);
	}

	if (!_CreateD3D11Device(dxgiAdapter)) {
		return false;
	}

	winrt::com_ptr<IDXGIDevice> dxgiDevice;
	HRESULT hr = _d3d11Device->QueryInterface<IDXGIDevice>(dxgiDevice.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("获取 IDXGIDevice 失败", hr);
		return false;
	}

	hr = CreateDirect3D11DeviceFromDXGIDevice(
		dxgiDevice.get(),
		reinterpret_cast<::IInspectable**>(winrt::put_abi(_wrappedD3DDevice))
	);
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
		ScalingWindow::Get().SrcTracker().Handle(),
		winrt::guid_of<winrt::GraphicsCaptureItem>(),
		winrt::put_abi(_captureItem)
	);
	if (FAILED(hr)) {
		Logger::Get().ComError("IGraphicsCaptureItemInterop::CreateForWindow 失败", hr);
		return false;
	}

	const uint32_t frameCount = ScalingWindow::Get().Options().maxProducerFramesInFlight;
	_framesInUse.reserve(frameCount);
	for (uint32_t i = 0; i < frameCount; ++i) {
		_framesInUse.emplace_back(nullptr);
	}

	_producerThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);

	CD3DX12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE_DEFAULT);
	CD3DX12_RESOURCE_DESC texDesc = CD3DX12_RESOURCE_DESC::Tex2D(
		useScRGB ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_TYPELESS,
		UINT64(_frameBox.right - _frameBox.left),
		_frameBox.bottom - _frameBox.top,
		1, 1, 1, 0,
		D3D12_RESOURCE_FLAG_NONE
	);
	hr = _renderingDevice->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE,
		&texDesc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&_output));
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateCommittedResource 失败", hr);
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
			_wrappedD3DDevice,
			_isUsingScRGB ? winrt::DirectXPixelFormat::R16G16B16A16Float : winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized,
			ScalingWindow::Get().Options().maxProducerFramesInFlight + 3,
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

bool GraphicsCaptureFrameSource2::IsNewFrameAvailable() noexcept {
	auto lk = _newFrameLock.lock_shared();
	return _isNewFrameAvailable;
}

bool GraphicsCaptureFrameSource2::Update(ID3D12GraphicsCommandList* commandList, uint32_t frameIndex) noexcept {
	_CapturedFrame& currentFrame = _framesInUse[frameIndex];

	{
		// 不要在持有 _newFrameLock 时释放 Direct3D11CaptureFrame。WGC 内部使用 Critical Section
		// 同步，如果此时 _Direct3D11CaptureFramePool_FrameArrived 正在执行会死锁。
		winrt::Direct3D11CaptureFrame lastestFrame{ nullptr };
		{
			auto lk = _newFrameLock.lock_exclusive();
			if (currentFrame.frame != _lastestFrame) {
				lastestFrame = _lastestFrame;
				_isNewFrameAvailable = false;
			}
		}
		if (lastestFrame) {
			currentFrame.sharedResource = nullptr;
			currentFrame.frame = std::move(lastestFrame);
		}
	}

	if (!currentFrame.sharedResource) {
		winrt::IDirect3DSurface d3dSurface = currentFrame.frame.Surface();

		auto dxgiInterfaceAccess =
			d3dSurface.try_as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();

		winrt::com_ptr<ID3D11Texture2D> capturedFrame;
		HRESULT hr = dxgiInterfaceAccess->GetInterface(IID_PPV_ARGS(&capturedFrame));
		if (FAILED(hr)) {
			Logger::Get().ComError("IDirect3DDxgiInterfaceAccess::GetInterface 失败", hr);
			return false;
		}

		auto dxgiResource = capturedFrame.try_as<IDXGIResource1>();
		wil::unique_handle hSharedResource;
		hr = dxgiResource->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, hSharedResource.put());
		if (FAILED(hr)) {
			Logger::Get().ComError("IDXGIResource1::CreateSharedHandle 失败", hr);
			return false;
		}

		if (_bridgeDevice) {
			hr = _bridgeDevice->OpenSharedHandle(hSharedResource.get(), IID_PPV_ARGS(&currentFrame.sharedResource));
			if (FAILED(hr)) {
				Logger::Get().ComError("OpenSharedHandle 失败", hr);
				return false;
			}
		} else {
			hr = _renderingDevice->OpenSharedHandle(hSharedResource.get(), IID_PPV_ARGS(&currentFrame.sharedResource));
			if (FAILED(hr)) {
				Logger::Get().ComError("OpenSharedHandle 失败", hr);
				return false;
			}
		}
	}

	// 共享纹理有 D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS 标志，因此无需屏障
	D3D12_RESOURCE_BARRIER barrier = {
		.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
		.Transition = {
			.pResource = _output.get(),
			.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST
		}
	};
	commandList->ResourceBarrier(1, &barrier);

	CD3DX12_TEXTURE_COPY_LOCATION src(currentFrame.sharedResource.get(), 0);
	CD3DX12_TEXTURE_COPY_LOCATION dest(_output.get(), 0);
	commandList->CopyTextureRegion(&dest, 0, 0, 0, &src, &_frameBox);

	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	commandList->ResourceBarrier(1, &barrier);

	return true;
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

bool GraphicsCaptureFrameSource2::_CreateD3D11Device(IDXGIAdapter1* dxgiAdapter) noexcept {
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
		dxgiAdapter,
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

bool GraphicsCaptureFrameSource2::_CreateBridgeDeviceResources(IDXGIAdapter1* dxgiAdapter) noexcept {
	HRESULT hr = D3D12CreateDevice(dxgiAdapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&_bridgeDevice));
	if (FAILED(hr)) {
		Logger::Get().ComError("D3D12CreateDevice 失败", hr);
		return false;
	}

	Logger::Get().Info("已创建 D3D12 设备");

	{
		// 只需要复制
		D3D12_COMMAND_QUEUE_DESC queueDesc = { .Type = D3D12_COMMAND_LIST_TYPE_COPY };
		hr = _bridgeDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&_bridgeCommandQueue));
		if (FAILED(hr)) {
			Logger::Get().ComError("CreateCommandQueue 失败", hr);
			return false;
		}
	}
	
	hr = _bridgeDevice->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_COPY,
		D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(&_bridgeCommandList));
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateCommandList1 失败", hr);
		return false;
	}

	const uint32_t frameCount = ScalingWindow::Get().Options().maxProducerFramesInFlight;
	_bridgeCommandAllocators.resize(frameCount);
	for (winrt::com_ptr<ID3D12CommandAllocator>& commandAllocator : _bridgeCommandAllocators) {
		if (FAILED(_bridgeDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&commandAllocator)))) {
			return false;
		}
	}

	CD3DX12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE_DEFAULT);
	CD3DX12_RESOURCE_DESC texDesc = CD3DX12_RESOURCE_DESC::Tex2D(
		_isUsingScRGB ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM,
		UINT64(_frameBox.right - _frameBox.left),
		_frameBox.bottom - _frameBox.top,
		1, 1, 1, 0,
		D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER,
		D3D12_TEXTURE_LAYOUT_ROW_MAJOR
	);

	_bridgeResources.resize(frameCount);
	for (winrt::com_ptr<ID3D12Resource>& resouce : _bridgeResources) {
		hr = _renderingDevice->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER,
			&texDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&resouce));
		if (FAILED(hr)) {
			Logger::Get().ComError("CreateCommittedResource 失败", hr);
			return false;
		}
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
		auto lk = _newFrameLock.lock_exclusive();
		_lastestFrame = std::move(frame);
		_isNewFrameAvailable = true;
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

#include "pch.h"
#include "GraphicsCaptureFrameSource.h"
#include "DebugInfo.h"
#include "DirectXHelper.h"
#include "DuplicateFrameChecker.h"
#include "GraphicsContext.h"
#include "Logger.h"
#include "ScalingWindow.h"
#include "Win32Helper.h"
#include "RectHelper.h"
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

static bool IsCornerInRect(Point p, const Rect& r) noexcept {
	return p.x >= r.left && p.x <= r.right && p.y >= r.top && p.y <= r.bottom;
}

static bool OptimizeDirtyRectPair(Rect& rect1, Rect& rect2, bool reversed = false) noexcept {
	if (RectHelper::IsEmpty(rect1) || RectHelper::IsEmpty(rect2)) {
		return false;
	}

	// 计算 rect2 有几个角在 rect1 内
	bool lt = IsCornerInRect(Point{ rect2.left, rect2.top }, rect1);
	bool rt = IsCornerInRect(Point{ rect2.right, rect2.top }, rect1);
	bool rb = IsCornerInRect(Point{ rect2.right, rect2.bottom }, rect1);
	bool lb = IsCornerInRect(Point{ rect2.left, rect2.bottom }, rect1);
	uint32_t count = (uint32_t)lt + (uint32_t)rt + (uint32_t)rb + (uint32_t)lb;

	if (count == 0) {
		// 尝试反向
		if (!reversed) {
			return OptimizeDirtyRectPair(rect2, rect1, true);
		}
	} else if (count == 2) {
		// rect2 有两个角在 rect1 内时可以合并或裁剪
		if (lt) {
			if (rt) {
				if (rect2.left == rect1.left && rect2.right == rect1.right) {
					// rect2 合并进 rect1
					rect1.bottom = rect2.bottom;
					rect2.right = rect2.left;
					return true;
				} else if (rect2.top != rect1.bottom) {
					// 裁剪 rect2
					rect2.top = rect1.bottom;
					assert(rect2.bottom >= rect2.top);
					return true;
				}
			} else {
				assert(lb);
				if (rect2.top == rect1.top && rect2.bottom == rect1.bottom) {
					rect1.right = rect2.right;
					rect2.right = rect2.left;
					return true;
				} else if (rect2.left != rect1.right) {
					rect2.left = rect1.right;
					assert(rect2.right >= rect2.left);
					return true;
				}
			}
		} else {
			assert(rb);
			if (rt) {
				if (rect2.top == rect1.top && rect2.bottom == rect1.bottom) {
					rect1.left = rect2.left;
					rect2.right = rect2.left;
					return true;
				} else if (rect2.right != rect1.left) {
					rect2.right = rect1.left;
					assert(rect2.right >= rect2.left);
					return true;
				}
			} else {
				if (rect2.left == rect1.left && rect2.right == rect1.right) {
					rect1.top = rect2.top;
					rect2.right = rect2.left;
					return true;
				} else if (rect2.bottom != rect1.top) {
					rect2.bottom = rect1.top;
					assert(rect2.bottom >= rect2.top);
					return true;
				}
			}
		}
	} else if (count == 4) {
		// rect2 在 rect1 内
		rect2.right = rect2.left;
		return true;
	}

	return false;
}

static void OptimizeDirtyRects(SmallVectorImpl<Rect>& dirtyRects) noexcept {
	// 持续循环直到不再能优化
	while (true) {
		const uint32_t count = (uint32_t)dirtyRects.size();
		assert(count > 0);

		bool optimized = false;
		for (uint32_t i = 0; i < count; ++i) {
			for (uint32_t j = i + 1; j < count; ++j) {
				if (OptimizeDirtyRectPair(dirtyRects[i], dirtyRects[j])) {
					optimized = true;
				}
			}
		}

		if (!optimized) {
			return;
		}

		// 从后向前删除空矩形
		for (int i = int(count - 1); i >= 0; --i) {
			const Rect& rect = dirtyRects[i];
			if (RectHelper::IsEmpty(rect)) {
				dirtyRects.erase(dirtyRects.begin() + i);
			}
		}
	}
}

static void OptimizeDirtyRects2(SmallVectorImpl<Rect>& dirtyRects) noexcept {
	OptimizeDirtyRects(dirtyRects);

	while (true) {
		const uint32_t count = (uint32_t)dirtyRects.size();

		uint32_t originTotalPixels = 0;
		for (uint32_t i = 0; i < count; ++i) {
			originTotalPixels += RectHelper::CalcArea(dirtyRects[i]);
		}

		uint32_t minTotalPixels = std::numeric_limits<uint32_t>::max();
		uint32_t targetRectCount = 0;
		uint32_t targetI = 0;
		uint32_t targetJ = 0;
		// 遍历所有两两合并的方式找出总像素数最少的
		for (uint32_t i = 0; i < count; ++i) {
			for (uint32_t j = i + 1; j < count; ++j) {
				Rect unionedRect = RectHelper::Union(dirtyRects[i], dirtyRects[j]);
				uint32_t totalPixels = 0;
				uint32_t rectCount = 0;

				// 为了降低复杂度这里只优化一轮，而不是调用 OptimizeDirtyRects
				for (uint32_t k = 0; k < count; ++k) {
					if (k == i || k == j) {
						continue;
					}

					Rect curRect = dirtyRects[k];
					OptimizeDirtyRectPair(curRect, unionedRect);

					if (!RectHelper::IsEmpty(curRect)) {
						totalPixels += RectHelper::CalcArea(curRect);
						++rectCount;
					}
				}

				if (!RectHelper::IsEmpty(unionedRect)) {
					totalPixels += RectHelper::CalcArea(unionedRect);
					++rectCount;
				}

				if (totalPixels < minTotalPixels || (totalPixels == minTotalPixels && rectCount < targetRectCount)) {
					minTotalPixels = totalPixels;
					targetRectCount = rectCount;
					targetI = i;
					targetJ = j;
				}
			}
		}

		if (minTotalPixels > originTotalPixels && count <= MAX_CAPTURE_DIRTY_RECTS) {
			break;
		}

		Rect unionedRect = RectHelper::Union(dirtyRects[targetI], dirtyRects[targetJ]);
		dirtyRects.erase(dirtyRects.begin() + targetJ);
		dirtyRects.erase(dirtyRects.begin() + targetI);
		dirtyRects.push_back(unionedRect);

		OptimizeDirtyRects(dirtyRects);

		if (minTotalPixels > originTotalPixels && dirtyRects.size() <= MAX_CAPTURE_DIRTY_RECTS) {
			break;
		}
	}
}

#ifdef _DEBUG
static Ignore _ = [] {
	auto rectComp = [](const Rect& l, const Rect& r) {
		return std::tuple(l.left, l.top, l.right, l.bottom) <
			std::tuple(r.left, r.top, r.right, r.bottom);
	};

	SmallVector<Rect, 0> dirtyRects;
	dirtyRects.reserve(16);

	dirtyRects.emplace_back(0, 0, 2, 2);
	dirtyRects.emplace_back(1, 1, 3, 4);
	dirtyRects.emplace_back(2, 1, 4, 3);
	dirtyRects.emplace_back(0, 1, 3, 2);
	dirtyRects.emplace_back(3, 3, 4, 4);

	OptimizeDirtyRects(dirtyRects);

	std::sort(dirtyRects.begin(), dirtyRects.end(), rectComp);
	assert(dirtyRects.size() == 2);
	assert((dirtyRects[0] == Rect{ 0, 0, 2, 2 }));
	assert((dirtyRects[1] == Rect{ 1, 1, 4, 4 }));

	return Ignore();
}();
#endif

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
	_isScRGB = colorInfo.kind != winrt::AdvancedColorKind::StandardDynamicRange;

	if (!winrt::GraphicsCaptureSession::IsSupported()) {
		Logger::Get().Error("当前无法使用 Graphics Capture");
		return false;
	}

	// 从 Win11 24H2 开始支持
	_isDirtyRegionSupported = winrt::ApiInformation::IsPropertyPresent(
		winrt::name_of<winrt::GraphicsCaptureSession>(), L"DirtyRegionMode");

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
			_isScRGB ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_TYPELESS,
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
		_duplicateFrameChecker = std::make_unique<DuplicateFrameChecker>();
		if (!_duplicateFrameChecker->Initialize(
			_bridgeDevice ? _bridgeDevice.get() : device,
			MAX_CAPTURE_DIRTY_RECTS,
			colorInfo,
			Size{ _frameBox.right, _frameBox.bottom })
		) {
			Logger::Get().Error("DuplicateFrameChecker::Initialize 失败");
			return false;
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
			_newFrameDirtyRects = std::move(_latestFrameDirtyRects);
			_latestFrame = nullptr;
		} else {
			isNewFrameAvailable = (bool)_newFrame;
			return S_OK;
		}
	}
	
	ID3D12Device5* dfDevice = _bridgeDevice ? _bridgeDevice.get() : _graphicsContext->GetDevice();

	HRESULT hr = GetFrameResourceFromCaptureFrame(_newFrame, dfDevice, _newFrameResource);
	if (FAILED(hr)) {
		Logger::Get().ComError("GetFrameResourceFromCaptureFrame 失败", hr);
		return hr;
	}

	if (!_duplicateFrameChecker) {
		isNewFrameAvailable = true;
		return S_OK;
	}

	// 不支持脏矩形时检查整个捕获区域
	if (!_isDirtyRegionSupported) {
		_newFrameDirtyRects.emplace_back(_frameBox.left, _frameBox.top, _frameBox.right, _frameBox.bottom);
	}

	hr = _duplicateFrameChecker->CheckFrame(_newFrameResource.get(), _newFrameDirtyRects);
	if (FAILED(hr)) {
		Logger::Get().ComError("DuplicateFrameChecker::CheckFrame 失败", hr);
		return hr;
	}

	isNewFrameAvailable = !_newFrameDirtyRects.empty();
	if (!isNewFrameAvailable) {
		_newFrame = nullptr;
		_newFrameResource = nullptr;
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

	if (_duplicateFrameChecker) {
		_duplicateFrameChecker->OnFrameAdopted();
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
		_isScRGB ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM,
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
	winrt::Direct3D11CaptureFrame frame{ nullptr };
	SmallVector<Rect> dirtyRects;

	// 取最新帧
	while (true) {
		winrt::Direct3D11CaptureFrame nextFrame = pool.TryGetNextFrame();
		if (!nextFrame) {
			break;
		}

		frame = std::move(nextFrame);

		if (_isDirtyRegionSupported) {
			for (const winrt::RectInt32& dirtyRect : frame.DirtyRegions()) {
				RECT clipped = {
					std::max(dirtyRect.X, (int)_frameBox.left),
					std::max(dirtyRect.Y, (int)_frameBox.top),
					std::min(dirtyRect.X + dirtyRect.Width, (int)_frameBox.right),
					std::min(dirtyRect.Y + dirtyRect.Height, (int)_frameBox.bottom),
				};
				if (clipped.right <= clipped.left || clipped.bottom <= clipped.top) {
					continue;
				}

				dirtyRects.emplace_back((uint32_t)clipped.left, (uint32_t)clipped.top,
					(uint32_t)clipped.right, (uint32_t)clipped.bottom);
			}
		}
	}

	if (!frame || (_isDirtyRegionSupported && dirtyRects.empty())) {
		return;
	}

	if (dirtyRects.size() > 1) {
		OptimizeDirtyRects2(dirtyRects);
	}

	{
		auto lk = _latestFrameLock.lock_exclusive();
		_latestFrame = std::move(frame);
		_latestFrameDirtyRects = std::move(dirtyRects);

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
			_isScRGB ? winrt::DirectXPixelFormat::R16G16B16A16Float : winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized,
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

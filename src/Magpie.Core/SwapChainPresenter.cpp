#include "pch.h"
#include "SwapChainPresenter.h"
#include "ColorInfo.h"
#include "GraphicsContext.h"
#include "Logger.h"
#include "Win32Helper.h"
#include <dcomp.h>
#include <dwmapi.h>

namespace Magpie {

bool SwapChainPresenter::Initialize(
	GraphicsContext& graphicContext,
	HWND hwndAttach,
	struct Size size,
	const ColorInfo& colorInfo
) noexcept {
	_graphicContext = &graphicContext;
	_size = size;
	_isScRGB = colorInfo.kind != winrt::AdvancedColorKind::StandardDynamicRange;

	IDXGIFactory7* dxgiFactory = graphicContext.GetDXGIFactory();
	ID3D12Device5* device = graphicContext.GetDevice();

	// 检查撕裂支持
	{
		BOOL supportTearing = FALSE;
		HRESULT hr = dxgiFactory->CheckFeatureSupport(
			DXGI_FEATURE_PRESENT_ALLOW_TEARING, &supportTearing, sizeof(supportTearing));
		if (FAILED(hr)) {
			Logger::Get().ComError("IDXGIFactory5::CheckFeatureSupport 失败", hr);
			return false;
		}

		_isTearingSupported = supportTearing;
	}

	_bufferCount = graphicContext.GetMaxInFlightFrameCount() + 1;

	DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {
		.Width = size.width,
		.Height = size.height,
		// 默认色域正是我们想要的，无需额外设置
		.Format = _isScRGB ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM,
		.SampleDesc = {
			.Count = 1
		},
		.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
		.BufferCount = _bufferCount,
#ifdef _DEBUG
		// 使边缘闪烁更容易观察到
		.Scaling = DXGI_SCALING_NONE,
#else
		// 使视觉变化尽可能小
		.Scaling = DXGI_SCALING_STRETCH,
#endif
		// 渲染每帧之前都会清空后缓冲区，因此无需 DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL
		.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
		.AlphaMode = DXGI_ALPHA_MODE_IGNORE,
		// 支持时始终启用 DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING
		.Flags = UINT((_isTearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0)
		| DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)
	};

	winrt::com_ptr<IDXGISwapChain1> dxgiSwapChain;
	HRESULT hr = dxgiFactory->CreateSwapChainForHwnd(
		graphicContext.GetCommandQueue(),
		hwndAttach,
		&swapChainDesc,
		nullptr,
		nullptr,
		dxgiSwapChain.put()
	);
	if (FAILED(hr)) {
		Logger::Get().ComError("IDXGIFactory2::CreateSwapChainForHwnd 失败", hr);
		return false;
	}

	_dxgiSwapChain = dxgiSwapChain.try_as<IDXGISwapChain4>();
	if (!_dxgiSwapChain) {
		Logger::Get().Error("检索 IDXGISwapChain4 失败");
		return false;
	}

	hr = _dxgiSwapChain->SetMaximumFrameLatency(_bufferCount - 1);
	if (FAILED(hr)) {
		Logger::Get().ComError("IDXGISwapChain2::SetMaximumFrameLatency 失败", hr);
		return false;
	}

	_frameLatencyWaitableObject.reset(_dxgiSwapChain->GetFrameLatencyWaitableObject());
	if (!_frameLatencyWaitableObject) {
		Logger::Get().Error("IDXGISwapChain2::GetFrameLatencyWaitableObject 失败");
		return false;
	}

	dxgiFactory->MakeWindowAssociation(hwndAttach, DXGI_MWA_NO_ALT_ENTER);

	{
		D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {
			.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
			.NumDescriptors = _bufferCount
		};
		hr = device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&_rtvHeap));
		if (FAILED(hr)) {
			Logger::Get().ComError("CreateDescriptorHeap 失败", hr);
			return false;
		}
	}

	_rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

	_frameBuffers.resize(_bufferCount);

	hr = _LoadBufferResources();
	if (FAILED(hr)) {
		Logger::Get().ComError("_LoadBufferResources 失败", hr);
		return false;
	}

	return true;
}

void SwapChainPresenter::BeginFrame(ID3D12Resource** frameTex, CD3DX12_CPU_DESCRIPTOR_HANDLE& rtvHandle) noexcept {
	_frameLatencyWaitableObject.wait(1000);

	const uint32_t curBufferIndex = _dxgiSwapChain->GetCurrentBackBufferIndex();
	*frameTex = _frameBuffers[curBufferIndex].get();
	rtvHandle = CD3DX12_CPU_DESCRIPTOR_HANDLE(
		_rtvHeap->GetCPUDescriptorHandleForHeapStart(), curBufferIndex, _rtvDescriptorSize);
}

// 和 DwmFlush 效果相同但更准确
static void WaitForDwmComposition() noexcept {
	// Win11 可以使用准确的 DCompositionWaitForCompositorClock
	if (Win32Helper::GetOSVersion().IsWin11()) {
		static const auto dCompositionWaitForCompositorClock =
			Win32Helper::LoadSystemFunction<decltype(DCompositionWaitForCompositorClock)>(
				L"dcomp.dll", "DCompositionWaitForCompositorClock");
		if (dCompositionWaitForCompositorClock) {
			dCompositionWaitForCompositorClock(0, nullptr, INFINITE);
			return;
		}
	}

	LARGE_INTEGER qpf;
	QueryPerformanceFrequency(&qpf);
	qpf.QuadPart /= 10000000;

	DWM_TIMING_INFO info{};
	info.cbSize = sizeof(info);
	DwmGetCompositionTimingInfo(NULL, &info);

	LARGE_INTEGER time;
	QueryPerformanceCounter(&time);

	if (time.QuadPart >= (LONGLONG)info.qpcCompose) {
		return;
	}

	// 提前 1ms 结束然后忙等待
	time.QuadPart += 10000;
	if (time.QuadPart < (LONGLONG)info.qpcCompose) {
		LARGE_INTEGER liDueTime{
			.QuadPart = -((LONGLONG)info.qpcCompose - time.QuadPart) / qpf.QuadPart
		};
		static HANDLE timer = CreateWaitableTimerEx(nullptr, nullptr,
			CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
		SetWaitableTimerEx(timer, &liDueTime, 0, NULL, NULL, 0, 0);
		WaitForSingleObject(timer, INFINITE);
	} else {
		Sleep(0);
	}

	while (true) {
		QueryPerformanceCounter(&time);

		if (time.QuadPart >= (LONGLONG)info.qpcCompose) {
			return;
		}

		Sleep(0);
	}
}

HRESULT SwapChainPresenter::EndFrame() noexcept {
	const bool isRecreated = std::exchange(_isRecreated, false);
	if (isRecreated) {
		// 下面两个调用用于减少调整窗口尺寸时的边缘闪烁。
		// 
		// 我们希望 DWM 绘制新的窗口框架时刚好合成新帧，但这不是我们能控制的，尤其是混合架构
		// 下需要在显卡间传输帧数据，无法预测 Present/Commit 后多久 DWM 能收到。我们只能尽
		// 可能为 DWM 合成新帧预留时间，这包括两个步骤：
		// 
		// 1. 首先等待渲染完成，确保新帧对 DWM 随时可用。
		// 2. 然后在新一轮合成开始时提交，这让 DWM 有更多时间合成新帧。
		// 
		// 目前看来除非像 UWP 一般有 DWM 协助，否则彻底摆脱闪烁是不可能的。
		// 
		// https://github.com/Blinue/Magpie/pull/1071#issuecomment-2718314731 讨论了 UWP
		// 调整尺寸的方法，测试表明可以彻底解决闪烁问题。不过它使用了很不稳定的私有接口，没有
		// 实用价值。

		// 等待渲染完成
		HRESULT hr = _graphicContext->WaitForGpu();
		if (FAILED(hr)) {
			Logger::Get().ComError("GraphicsContext::WaitForGPU", hr);
			return hr;
		}

		// 等待 DWM 开始合成新一帧
		WaitForDwmComposition();
	}

	return _dxgiSwapChain->Present(0, 0);
}

HRESULT SwapChainPresenter::OnSizeChanged(struct Size size) noexcept {
	_size = size;
	// 调整大小期间只用两个后备缓冲以提高流畅度并减少边缘闪烁
	_bufferCount = _isResizing ? 2 : _graphicContext->GetMaxInFlightFrameCount() + 1;

	HRESULT hr = _RecreateBuffers();
	if (FAILED(hr)) {
		Logger::Get().ComError("_RecreateBuffers 失败", hr);
	}

	return hr;
}

void SwapChainPresenter::OnResizeStarted() noexcept {
	// 尺寸变化时再重建交换链
	_isResizing = true;
}

HRESULT SwapChainPresenter::OnResizeEnded() noexcept {
	_isResizing = false;

	// 恢复后备缓冲数量
	const uint32_t oldBufferCount = _bufferCount;
	_bufferCount = _graphicContext->GetMaxInFlightFrameCount() + 1;

	if (_bufferCount == oldBufferCount) {
		return S_OK;
	}

	HRESULT hr = _RecreateBuffers();
	if (FAILED(hr)) {
		Logger::Get().ComError("_RecreateBuffers 失败", hr);
	}

	return hr;
}

HRESULT SwapChainPresenter::OnColorInfoChanged(const ColorInfo& colorInfo) noexcept {
	const bool wasScRGB = _isScRGB;
	_isScRGB = colorInfo.kind != winrt::AdvancedColorKind::StandardDynamicRange;

	if (_isScRGB == wasScRGB) {
		return S_OK;
	}

	HRESULT hr = _RecreateBuffers();
	if (FAILED(hr)) {
		Logger::Get().ComError("_RecreateBuffers 失败", hr);
		return hr;
	}

	hr = _dxgiSwapChain->SetColorSpace1(
		_isScRGB ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
	if (FAILED(hr)) {
		Logger::Get().ComError("IDXGISwapChain4::SetColorSpace1 失败", hr);
	}

	return hr;
}

HRESULT SwapChainPresenter::_RecreateBuffers() noexcept {
	HRESULT hr = _graphicContext->WaitForGpu();
	if (FAILED(hr)) {
		Logger::Get().ComError("GraphicsContext::WaitForGPU", hr);
		return hr;
	}

	std::fill(_frameBuffers.begin(), _frameBuffers.end(), nullptr);

	// 不要更改最大帧延迟，一来调整大小期间不会有帧排队，二来交换链不大支持中途改变
	// 最大帧延迟，需要额外等待 FrameLatencyWaitableObject 来修正内部状态。
	hr = _dxgiSwapChain->ResizeBuffers(
		_bufferCount, _size.width, _size.height,
		_isScRGB ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM,
		UINT((_isTearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0)
			| DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)
	);
	if (FAILED(hr)) {
		Logger::Get().ComError("IDXGISwapChain::ResizeBuffers", hr);
		return hr;
	}

	_isRecreated = true;

	hr = _LoadBufferResources();
	if (FAILED(hr)) {
		Logger::Get().ComError("_LoadBufferResources 失败", hr);
	}

	return hr;
}

HRESULT SwapChainPresenter::_LoadBufferResources() noexcept {
	ID3D12Device5* device = _graphicContext->GetDevice();
	CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(_rtvHeap->GetCPUDescriptorHandleForHeapStart());
	for (uint32_t i = 0; i < _bufferCount; ++i) {
		HRESULT hr = _dxgiSwapChain->GetBuffer(i, IID_PPV_ARGS(&_frameBuffers[i]));
		if (FAILED(hr)) {
			Logger::Get().ComError("IDXGISwapChain::GetBuffer 失败", hr);
			return hr;
		}

		D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {
			.Format = _isScRGB ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
			.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D
		};
		device->CreateRenderTargetView(_frameBuffers[i].get(), &rtvDesc, rtvHandle);
		rtvHandle.Offset(1, _rtvDescriptorSize);
	}

	return S_OK;
}

}

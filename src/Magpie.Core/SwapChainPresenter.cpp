#include "pch.h"
#include "SwapChainPresenter.h"
#include "Win32Helper.h"
#include "ScalingWindow.h"
#include <dcomp.h>
#include <dwmapi.h>

namespace Magpie {

static constexpr uint32_t BUFFER_COUNT_DURING_RESIZE = 2;

SwapChainPresenter::~SwapChainPresenter() {
	_WaitForGpu();
}

bool SwapChainPresenter::Initialize(
	ID3D12Device5* device,
	ID3D12CommandQueue* commandQueue,
	IDXGIFactory7* dxgiFactory,
	HWND hwndAttach,
	bool useScRGB
) noexcept {
	_device = device;
	_commandQueue = commandQueue;

	// 检查可变帧率支持
	BOOL supportTearing = FALSE;
	HRESULT hr = dxgiFactory->CheckFeatureSupport(
		DXGI_FEATURE_PRESENT_ALLOW_TEARING, &supportTearing, sizeof(supportTearing));
	if (FAILED(hr)) {
		Logger::Get().ComWarn("CheckFeatureSupport 失败", hr);
	}

	_isTearingSupported = supportTearing;
	Logger::Get().Info(fmt::format("可变刷新率支持: {}", supportTearing ? "是" : "否"));

	const uint32_t bufferCount = GetBufferCount();
	const RECT& rendererRect = ScalingWindow::Get().RendererRect();

	DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {
		.Width = UINT(rendererRect.right - rendererRect.left),
		.Height = UINT(rendererRect.bottom - rendererRect.top),
		.Format = useScRGB ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM,
		.SampleDesc = {
			.Count = 1
		},
		.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
		.BufferCount = bufferCount,
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
		// 只要显卡支持始终启用 DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING 以支持可变刷新率
		.Flags = UINT((supportTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0)
		| DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)
	};

	winrt::com_ptr<IDXGISwapChain1> dxgiSwapChain;
	if (FAILED(dxgiFactory->CreateSwapChainForHwnd(
		commandQueue,
		hwndAttach,
		&swapChainDesc,
		nullptr,
		nullptr,
		dxgiSwapChain.put()
	))) {
		return false;
	}

	_dxgiSwapChain = dxgiSwapChain.try_as<IDXGISwapChain4>();
	if (!_dxgiSwapChain) {
		return false;
	}

	// 两个垂直同步之间允许渲染 bufferCount - 1 帧
	if (FAILED(_dxgiSwapChain->SetMaximumFrameLatency(bufferCount - 1))) {
		return false;
	}

	_frameLatencyWaitableObject.reset(_dxgiSwapChain->GetFrameLatencyWaitableObject());
	if (!_frameLatencyWaitableObject) {
		return false;
	}

	_curBufferIndex = _dxgiSwapChain->GetCurrentBackBufferIndex();

	dxgiFactory->MakeWindowAssociation(hwndAttach, DXGI_MWA_NO_ALT_ENTER);

	{
		D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {
			.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
			.NumDescriptors = bufferCount
		};
		if (FAILED(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&_rtvHeap)))) {
			return false;
		}
	}

	_rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

	if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_fence)))) {
		return false;
	}

	_frameBuffers.resize(bufferCount);
	_frameBufferFenceValues.resize(bufferCount);

	return SUCCEEDED(_LoadBufferResources(bufferCount, useScRGB));
}

uint32_t SwapChainPresenter::GetBufferCount() const noexcept {
	// 缓冲区数量取决于 ScalingRuntime::_ScalingThreadProc 中检查光标移动的频率
	return ScalingWindow::Get().Options().Is3DGameMode() ? 4 : 8;
}

HRESULT SwapChainPresenter::BeginFrame(
	ID3D12Resource** frameTex,
	CD3DX12_CPU_DESCRIPTOR_HANDLE& rtvHandle,
	uint32_t& bufferIndex
) noexcept {
	_frameLatencyWaitableObject.wait(1000);

	if (_fence->GetCompletedValue() < _frameBufferFenceValues[_curBufferIndex]) {
		HRESULT hr = _fence->SetEventOnCompletion(_frameBufferFenceValues[_curBufferIndex], nullptr);
		if (FAILED(hr)) {
			return hr;
		}
	}

	*frameTex = _frameBuffers[_curBufferIndex].get();
	rtvHandle = CD3DX12_CPU_DESCRIPTOR_HANDLE(
		_rtvHeap->GetCPUDescriptorHandleForHeapStart(), _curBufferIndex, _rtvDescriptorSize);
	bufferIndex = _curBufferIndex;

	return S_OK;
}

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
	if (_isRecreated) {
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
		HRESULT hr = _WaitForGpu();
		if (FAILED(hr)) {
			return hr;
		}

		// 等待 DWM 开始合成新一帧
		WaitForDwmComposition();

		_isRecreated = false;
	}

	HRESULT hr = _dxgiSwapChain->Present(0, 0);
	if (FAILED(hr)) {
		return hr;
	}

	hr = _commandQueue->Signal(_fence.get(), ++_curFenceValue);
	if (FAILED(hr)) {
		return hr;
	}
	_frameBufferFenceValues[_curBufferIndex] = _curFenceValue;

	_curBufferIndex = _dxgiSwapChain->GetCurrentBackBufferIndex();

	return S_OK;
}

HRESULT SwapChainPresenter::RecreateBuffers(bool useScRGB) noexcept {
	HRESULT hr = _WaitForGpu();
	if (FAILED(hr)) {
		return hr;
	}

	// 调整大小期间只用两个后缓冲以提高流畅度并减少边缘闪烁
	const uint32_t bufferCount =
		ScalingWindow::Get().IsResizing() ? BUFFER_COUNT_DURING_RESIZE : GetBufferCount();

	std::fill(_frameBuffers.begin(), _frameBuffers.end(), nullptr);

	const RECT& rendererRect = ScalingWindow::Get().RendererRect();
	hr = _dxgiSwapChain->ResizeBuffers(
		bufferCount,
		UINT(rendererRect.right - rendererRect.left),
		UINT(rendererRect.bottom - rendererRect.top),
		useScRGB ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM,
		UINT((_isTearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0)
			| DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)
	);
	if (FAILED(hr)) {
		return hr;
	}

	_isRecreated = true;

	hr = _dxgiSwapChain->SetMaximumFrameLatency(bufferCount - 1);
	if (FAILED(hr)) {
		return hr;
	}

	hr = _dxgiSwapChain->SetColorSpace1(
		useScRGB ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
	if (FAILED(hr)) {
		return hr;
	}

	_curBufferIndex = _dxgiSwapChain->GetCurrentBackBufferIndex();

	return _LoadBufferResources(bufferCount, useScRGB);
}

HRESULT SwapChainPresenter::OnResizeEnded() noexcept {
	// 调整大小结束后立刻重建交换链
	DXGI_SWAP_CHAIN_DESC1 desc;
	HRESULT hr = _dxgiSwapChain->GetDesc1(&desc);
	if (FAILED(hr)) {
		return hr;
	}

	// 后缓冲数量不变则从未调整过尺寸，无需重建交换链
	if (desc.BufferCount != BUFFER_COUNT_DURING_RESIZE) {
		return S_OK;
	} else {
		return RecreateBuffers(desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT);
	}
}

HRESULT SwapChainPresenter::_LoadBufferResources(uint32_t bufferCount, bool useScRGB) noexcept {
	CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(_rtvHeap->GetCPUDescriptorHandleForHeapStart());
	for (uint32_t i = 0; i < bufferCount; ++i) {
		HRESULT hr = _dxgiSwapChain->GetBuffer(i, IID_PPV_ARGS(&_frameBuffers[i]));
		if (FAILED(hr)) {
			return hr;
		}

		D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {
			.Format = useScRGB ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
			.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D
		};
		_device->CreateRenderTargetView(_frameBuffers[i].get(), &rtvDesc, rtvHandle);
		rtvHandle.Offset(1, _rtvDescriptorSize);
	}

	return S_OK;
}

HRESULT SwapChainPresenter::_WaitForGpu() noexcept {
	if (!_fence) {
		return S_OK;
	}

	HRESULT hr = _commandQueue->Signal(_fence.get(), ++_curFenceValue);
	if (FAILED(hr)) {
		return hr;
	}

	return _fence->SetEventOnCompletion(_curFenceValue, nullptr);
}

}

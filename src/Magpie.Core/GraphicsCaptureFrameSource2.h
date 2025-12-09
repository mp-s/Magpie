#pragma once
#include "ColorInfo.h"
#include <winrt/Windows.Graphics.Capture.h>

namespace Magpie {

class GraphicsContext;

enum class FrameSourceWaitType {
	NoWait,
	WaitForMessage,
	WaitForFrame
};

class GraphicsCaptureFrameSource2 {
public:
	GraphicsCaptureFrameSource2() = default;
	GraphicsCaptureFrameSource2(const GraphicsCaptureFrameSource2&) = delete;
	GraphicsCaptureFrameSource2(GraphicsCaptureFrameSource2&&) = delete;

	~GraphicsCaptureFrameSource2() noexcept;

	bool Initialize(
		GraphicsContext& graphicsContext,
		const RECT& srcRect,
		HMONITOR hMonSrc,
		const ColorInfo& colorInfo
	) noexcept;

	bool Start() noexcept;

	FrameSourceWaitType WaitType() const noexcept {
		return FrameSourceWaitType::WaitForMessage;
	}

	bool IsNewFrameAvailable() noexcept;

	HRESULT Update(uint32_t frameIndex) noexcept;

	ID3D12Resource* GetOutput(uint32_t index) noexcept {
		return _slots[index].output.get();
	}

private:
	bool _CreateD3D11Device(IDXGIAdapter1* dxgiAdapter) noexcept;

	bool _CreateBridgeDeviceResources(IDXGIAdapter1* dxgiAdapter) noexcept;

	void _Direct3D11CaptureFramePool_FrameArrived(
		const winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool& pool,
		const winrt::IInspectable&
	);

	void _StopCapture() noexcept;

	GraphicsContext* _graphicsContext = nullptr;

	winrt::com_ptr<ID3D11Device5> _d3d11Device;
	winrt::com_ptr<ID3D11DeviceContext4> _d3d11DC;

	winrt::com_ptr<ID3D12CommandQueue> _copyCommandQueue;
	winrt::com_ptr<ID3D12GraphicsCommandList> _copyCommandList;

	// 用于跨适配器捕获
	winrt::com_ptr<ID3D12Device5> _bridgeDevice;
	
	winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice _wrappedD3DDevice{ nullptr };
	winrt::Windows::Graphics::Capture::GraphicsCaptureItem _captureItem{ nullptr };
	winrt::Windows::Graphics::Capture::GraphicsCaptureSession _captureSession{ nullptr };
	winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool _captureFramePool{ nullptr };

	wil::srwlock _newFrameLock;
	winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame _lastestFrame{ nullptr };
	bool _isNewFrameAvailable = false;

	std::atomic<DWORD> _producerThreadId;

	struct _FrameResourceSlot {
		winrt::com_ptr<ID3D12CommandAllocator> commandAllocator;
		// 保留引用防止 WGC 再次写入
		winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame frame{ nullptr };
		winrt::com_ptr<ID3D12Resource> copySource;
		winrt::com_ptr<ID3D12Resource> output;
	};

	std::vector<_FrameResourceSlot> _slots;
	
	D3D12_BOX _frameBox{};
	bool _isUsingScRGB = false;
};

}

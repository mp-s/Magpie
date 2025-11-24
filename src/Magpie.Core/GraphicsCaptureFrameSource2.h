#pragma once
#include <Windows.Graphics.Capture.Interop.h>
#include <winrt/Windows.Graphics.Capture.h>

namespace Magpie {

enum class FrameSourceWaitType {
	NoWait,
	WaitForMessage,
	WaitForFrame
};

class GraphicsCaptureFrameSource2 {
public:
	GraphicsCaptureFrameSource2() = default;
	~GraphicsCaptureFrameSource2();

	// 不可复制，不可移动
	GraphicsCaptureFrameSource2(const GraphicsCaptureFrameSource2&) = delete;
	GraphicsCaptureFrameSource2(GraphicsCaptureFrameSource2&&) = delete;

	bool Initialize(
		ID3D12Device5* device,
		IDXGIFactory7* dxgiFactory,
		IDXGIAdapter4* dxgiAdapter,
		HMONITOR hMonSrc,
		bool useScRGB
	) noexcept;

	bool Start() noexcept;

	FrameSourceWaitType WaitType() const noexcept {
		return FrameSourceWaitType::WaitForMessage;
	}

	bool IsNewFrameAvailable() noexcept;

	bool Update(ID3D12GraphicsCommandList* commandList, uint32_t frameIndex) noexcept;

	ID3D12Resource* GetOutput() noexcept {
		return _output.get();
	}

private:
	bool _CreateD3D11Device(IDXGIAdapter1* dxgiAdapter) noexcept;

	bool _CreateBridgeDeviceResources(IDXGIAdapter1* dxgiAdapter) noexcept;

	void _Direct3D11CaptureFramePool_FrameArrived(
		const winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool& pool,
		const winrt::IInspectable&
	);

	void _StopCapture() noexcept;

	ID3D12Device5* _renderingDevice = nullptr;

	winrt::com_ptr<ID3D11Device5> _d3d11Device;
	winrt::com_ptr<ID3D11DeviceContext4> _d3d11DC;

	// 用于跨适配器捕获
	winrt::com_ptr<ID3D12Device5> _bridgeDevice;
	winrt::com_ptr<ID3D12CommandQueue> _bridgeCommandQueue;
	winrt::com_ptr<ID3D12GraphicsCommandList> _bridgeCommandList;
	std::vector<winrt::com_ptr<ID3D12CommandAllocator>> _bridgeCommandAllocators;
	std::vector<winrt::com_ptr<ID3D12Resource>> _bridgeResources;

	winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice _wrappedD3DDevice{ nullptr };
	winrt::Windows::Graphics::Capture::GraphicsCaptureItem _captureItem{ nullptr };
	winrt::Windows::Graphics::Capture::GraphicsCaptureSession _captureSession{ nullptr };
	winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool _captureFramePool{ nullptr };

	wil::srwlock _newFrameLock;
	winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame _lastestFrame{ nullptr };
	bool _isNewFrameAvailable = false;

	std::atomic<DWORD> _producerThreadId;

	struct _CapturedFrame {
		winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame frame{ nullptr };
		winrt::com_ptr<ID3D12Resource> sharedResource;
	};
	std::vector<_CapturedFrame> _framesInUse;

	winrt::com_ptr<ID3D12Resource> _output;
	
	D3D12_BOX _frameBox{};
	bool _isUsingScRGB = false;
};

}

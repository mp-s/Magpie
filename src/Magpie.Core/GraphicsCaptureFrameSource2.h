#pragma once
#include <Windows.Graphics.Capture.Interop.h>
#include <winrt/Windows.Graphics.Capture.h>

namespace Magpie {

enum class FrameSourceState {
	NewFrame,
	Waiting,
	Error
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
		ID3D12CommandList* commandList,
		IDXGIFactory7* dxgiFactory,
		IDXGIAdapter4* dxgiAdapter,
		HMONITOR hMonSrc,
		bool useScRGB
	) noexcept;

	bool Start() noexcept;

	FrameSourceState Update(uint32_t frameIndex) noexcept;

	ID3D12Resource* GetOutput() noexcept {
		return _output.get();
	}

private:
	bool _CreateD3D11Device(IDXGIAdapter1* dxgiAdapter) noexcept;

	void _StopCapture() noexcept;

	winrt::com_ptr<ID3D11Device5> _d3d11Device;
	winrt::com_ptr<ID3D11DeviceContext4> _d3d11DC;

	winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice _wrappedD3DDevice{ nullptr };
	winrt::Windows::Graphics::Capture::GraphicsCaptureItem _captureItem{ nullptr };
	winrt::Windows::Graphics::Capture::GraphicsCaptureSession _captureSession{ nullptr };
	winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool _captureFramePool{ nullptr };

	wil::srwlock _lastestFrameLock;
	winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame _lastestFrame{ nullptr };

	std::vector<winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame> _framesInUse;

	winrt::com_ptr<ID3D12Resource> _output;
	
	D3D11_BOX _frameBox{};
};

}

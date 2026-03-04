#pragma once

namespace Magpie {

class GraphicsContext;

class CatmullRomDrawer {
public:
	void Initialize(GraphicsContext& graphicsContext) noexcept;

	HRESULT Draw(
		Size inputSize,
		Size outputSize,
		D3D12_GPU_DESCRIPTOR_HANDLE inputGpuHandle,
		D3D12_GPU_DESCRIPTOR_HANDLE outputGpuHandle,
		bool outputSrgb
	) noexcept;

private:
	GraphicsContext* _graphicsContext = nullptr;

	HRESULT _InitializeCatmullRomRootSignature() noexcept;
	HRESULT _InitializeCopyRootSignature() noexcept;

	winrt::com_ptr<ID3D12RootSignature> _catmullRomRootSignature;
	winrt::com_ptr<ID3D12PipelineState> _catmullRomPSO;
	winrt::com_ptr<ID3D12PipelineState> _catmullRomSrgbPSO;

	winrt::com_ptr<ID3D12RootSignature> _copyRootSignature;
	winrt::com_ptr<ID3D12PipelineState> _copyPSO;
	winrt::com_ptr<ID3D12PipelineState> _copySrgbPSO;
};

}

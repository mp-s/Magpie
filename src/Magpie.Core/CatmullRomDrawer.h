#pragma once

namespace Magpie {

class GraphicsContext;

class CatmullRomDrawer {
public:
	HRESULT Initialize(GraphicsContext& graphicsContext) noexcept;

	HRESULT Draw(
		Size inputSize,
		Size outputSize,
		D3D12_GPU_DESCRIPTOR_HANDLE inputGpuHandle,
		D3D12_GPU_DESCRIPTOR_HANDLE outputGpuHandle,
		bool outputSrgb
	) noexcept;

private:
	GraphicsContext* _graphicsContext = nullptr;

	winrt::com_ptr<ID3D12RootSignature> _rootSignature;
	winrt::com_ptr<ID3D12PipelineState> _linearPipelineState;
	winrt::com_ptr<ID3D12PipelineState> _srgbPipelineState;
};

}

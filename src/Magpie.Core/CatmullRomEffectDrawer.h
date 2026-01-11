#pragma once
#include "EffectDrawerBase.h"

namespace Magpie {

class GraphicsContext;

class CatmullRomEffectDrawer : public EffectDrawerBase {
public:
	virtual ~CatmullRomEffectDrawer() noexcept = default;

	static uint32_t CalcDescriptorCount(bool isFirst, bool isLast);

	HRESULT Initialize(
		GraphicsContext& graphicsContext,
		Size inputSize,
		Size outputSize,
		EffectColorSpace inputColorSpace,
		EffectColorSpace outputColorSpace,
		CD3DX12_CPU_DESCRIPTOR_HANDLE& descriptorCpuHandle,
		CD3DX12_GPU_DESCRIPTOR_HANDLE& descriptorGpuHandle,
		uint32_t descriptorSize,
		ID3D12Resource* inputResource,
		ID3D12Resource* outputResource
	) noexcept;

	void Draw(
		D3D12_GPU_DESCRIPTOR_HANDLE inputGpuHandle,
		D3D12_GPU_DESCRIPTOR_HANDLE outputGpuHandle
	) noexcept;

	void OnResized(
		Size inputSize,
		Size outputSize,
		CD3DX12_CPU_DESCRIPTOR_HANDLE& descriptorCpuHandle,
		CD3DX12_GPU_DESCRIPTOR_HANDLE& descriptorGpuHandle,
		uint32_t descriptorSize,
		ID3D12Resource* inputResource,
		ID3D12Resource* outputResource
	) noexcept;

	HRESULT OnColorInfoChanged(
		EffectColorSpace inputColorSpace,
		EffectColorSpace outputColorSpace,
		CD3DX12_CPU_DESCRIPTOR_HANDLE& descriptorCpuHandle,
		CD3DX12_GPU_DESCRIPTOR_HANDLE& descriptorGpuHandle,
		uint32_t descriptorSize,
		ID3D12Resource* inputResource,
		ID3D12Resource* outputResource
	) noexcept;

private:
	HRESULT _CreatePiplineState() noexcept;

	void _CreateDescriptors(
		CD3DX12_CPU_DESCRIPTOR_HANDLE& descriptorCpuHandle,
		CD3DX12_GPU_DESCRIPTOR_HANDLE& descriptorGpuHandle,
		uint32_t descriptorSize,
		ID3D12Resource* inputResource,
		ID3D12Resource* outputResource
	) noexcept;

	GraphicsContext* _graphicsContext = nullptr;

	Size _inputSize{};
	Size _outputSize{};
	EffectColorSpace _inputColorSpace = EffectColorSpace::linear_sRGB;
	EffectColorSpace _outputColorSpace = EffectColorSpace::linear_sRGB;

	winrt::com_ptr<ID3D12RootSignature> _rootSignature;
	winrt::com_ptr<ID3D12PipelineState> _pipelineState;

	CD3DX12_GPU_DESCRIPTOR_HANDLE _inputGpuHandle{};
	CD3DX12_GPU_DESCRIPTOR_HANDLE _outputGpuHandle{};
};

}

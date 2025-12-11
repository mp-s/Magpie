#pragma once
#include "EffectDrawerBase.h"
#include "ColorInfo.h"
#include "SmallVector.h"

namespace Magpie {

class GraphicsContext;

enum class EffectColorSpace {
	linear_sRGB,
	sRGB,
	scRGB
};

class CatumullRomEffectDrawer : public EffectDrawerBase {
public:
	virtual ~CatumullRomEffectDrawer() noexcept = default;

	HRESULT Initialize(
		GraphicsContext& graphicsContext,
		Size inputSize,
		Size outputSize,
		bool isFirst,
		EffectColorSpace inputColorSpace,
		EffectColorSpace outputColorSpace,
		uint32_t inputSlotCount,
		uint32_t outputSlotCount,
		uint32_t& descriptorCount
	) noexcept;

	HRESULT CreateDeviceResources(
		const SmallVectorImpl<ID3D12Resource*>& inputSlots,
		const SmallVectorImpl<ID3D12Resource*>& outputSlots,
		CD3DX12_CPU_DESCRIPTOR_HANDLE& descriptorCpuHandle,
		CD3DX12_GPU_DESCRIPTOR_HANDLE& descriptorGpuHandle,
		uint32_t descriptorSize
	) noexcept;

	HRESULT Draw(uint32_t inputSlot, uint32_t outputSlot) noexcept;

private:
	GraphicsContext* _graphicsContext = nullptr;

	Size _inputSize{};
	Size _outputSize{};
	EffectColorSpace _inputColorSpace = EffectColorSpace::linear_sRGB;
	EffectColorSpace _outputColorSpace = EffectColorSpace::linear_sRGB;

	winrt::com_ptr<ID3D12RootSignature> _rootSignature;
	winrt::com_ptr<ID3D12PipelineState> _pipelineState;

	CD3DX12_GPU_DESCRIPTOR_HANDLE _inputDescriptorBase{};
	CD3DX12_GPU_DESCRIPTOR_HANDLE _outputDescriptorBase{};
	uint32_t _descriptorSize = 0;

	bool _isFirst = false;
};

}

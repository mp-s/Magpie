#pragma once
#include "CatmullRomEffectDrawer.h"
#include "SmallVector.h"

namespace Magpie {

class EffectDrawerBase;

class EffectsDrawer {
public:
	EffectsDrawer() noexcept = default;
	EffectsDrawer(const EffectsDrawer&) = delete;
	EffectsDrawer(EffectsDrawer&&) = delete;

	bool Initialize(
		GraphicsContext& graphicsContext,
		const ColorInfo& colorInfo,
		Size inputSize,
		Size rendererSize,
		const SmallVectorImpl<ID3D12Resource*>& inputResources,
		SmallVectorImpl<winrt::com_ptr<ID3D12Resource>>& outputResources
	) noexcept;

	HRESULT Draw(uint32_t frameIndex, uint32_t inputIndx, uint32_t outputIndex) noexcept;

	Size GetOutputSize() const noexcept {
		return _outputSize;
	}

	HRESULT OnResized(
		Size rendererSize,
		SmallVectorImpl<winrt::com_ptr<ID3D12Resource>>& outputResources
	) noexcept;

	HRESULT OnColorInfoChanged(
		const ColorInfo& colorInfo,
		const SmallVectorImpl<ID3D12Resource*>& inputResources,
		SmallVectorImpl<winrt::com_ptr<ID3D12Resource>>& outputResources
	) noexcept;

private:
	HRESULT _CreateInputResources(
		const SmallVectorImpl<ID3D12Resource*>& inputResources
	) const noexcept;

	HRESULT _CreateOutputResources(
		SmallVectorImpl<winrt::com_ptr<ID3D12Resource>>& outputResources
	) const noexcept;

	GraphicsContext* _graphicsContext = nullptr;

	Size _inputSize{};
	Size _outputSize{};

	winrt::com_ptr<ID3D12DescriptorHeap> _descriptorHeap;
	uint32_t _descriptorSize = 0;
	CD3DX12_CPU_DESCRIPTOR_HANDLE _inputDescriptorCpuBase{};
	CD3DX12_GPU_DESCRIPTOR_HANDLE _inputDescriptorGpuBase{};
	CD3DX12_CPU_DESCRIPTOR_HANDLE _outputDescriptorCpuBase{};
	CD3DX12_GPU_DESCRIPTOR_HANDLE _outputDescriptorGpuBase{};
	CD3DX12_CPU_DESCRIPTOR_HANDLE _effectsDescriptorCpuBase{};
	CD3DX12_GPU_DESCRIPTOR_HANDLE _effectsDescriptorGpuBase{};

	CatmullRomEffectDrawer _catmullRom;

	winrt::com_ptr<ID3D12QueryHeap> _queryHeap;
	winrt::com_ptr<ID3D12Resource> _queryResultBuffer;
	UINT64 _timestampFrequency = 0;

	bool _isScRGB = false;
	bool _isFP16Supported = false;
};

}

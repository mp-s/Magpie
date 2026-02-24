#pragma once
#include "CatmullRomDrawer.h"
#include "SmallVector.h"

namespace Magpie {

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
		SmallVectorImpl<winrt::com_ptr<ID3D12Resource>>& outputResources
	) noexcept;

	HRESULT Draw(
		uint32_t frameIndex,
		D3D12_GPU_DESCRIPTOR_HANDLE inputSrvHandle,
		D3D12_GPU_DESCRIPTOR_HANDLE outputUavHandle
	) noexcept;

	Size GetOutputSize() const noexcept {
		return _outputSize;
	}

	HRESULT OnResized(
		Size rendererSize,
		SmallVectorImpl<winrt::com_ptr<ID3D12Resource>>& outputResources
	) noexcept;

	HRESULT OnColorInfoChanged(
		const ColorInfo& colorInfo,
		SmallVectorImpl<winrt::com_ptr<ID3D12Resource>>& outputResources
	) noexcept;

private:
	HRESULT _CreateOutputResources(
		SmallVectorImpl<winrt::com_ptr<ID3D12Resource>>& outputResources
	) noexcept;

	GraphicsContext* _graphicsContext = nullptr;

	Size _inputSize{};
	Size _outputSize{};

	std::optional<CatmullRomDrawer> _catmullRomDrawer;

	winrt::com_ptr<ID3D12QueryHeap> _queryHeap;
	winrt::com_ptr<ID3D12Resource> _queryResultBuffer;
	UINT64 _timestampFrequency = 0;

	bool _isScRGB = false;
	bool _isFP16Supported = false;
};

}

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
		Size rendererSize
	) noexcept;

	HRESULT Draw(
		uint32_t frameIndex,
		ID3D12Resource* inputResource,
		ID3D12Resource* outputResource,
		D3D12_GPU_DESCRIPTOR_HANDLE inputSrvHandle,
		D3D12_GPU_DESCRIPTOR_HANDLE outputUavHandle
	) noexcept;

	Size GetOutputSize() const noexcept {
		return _outputSize;
	}

	void OnResized(Size rendererSize) noexcept;

	void OnColorInfoChanged(const ColorInfo& colorInfo) noexcept;

private:
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

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

	HRESULT Draw(uint32_t frameIndex, uint32_t inputSrvIdx, uint32_t outputUavIdx) noexcept;

	Size GetOutputSize() const noexcept {
		return _outputSize;
	}

	HRESULT OnResized(Size rendererSize) noexcept;

	HRESULT OnColorInfoChanged(const ColorInfo& colorInfo) noexcept;

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

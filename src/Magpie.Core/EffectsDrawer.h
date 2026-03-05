#pragma once
#include "CatmullRomDrawer.h"
#ifdef MP_ENABLE_RTX_TRUE_HDR
#include "RtxTrueHdrDrawer.h"
#endif

namespace Magpie {

class EffectsDrawer {
public:
	EffectsDrawer() noexcept = default;
	EffectsDrawer(const EffectsDrawer&) = delete;
	EffectsDrawer(EffectsDrawer&&) = delete;

	~EffectsDrawer() noexcept;

	bool Initialize(
		GraphicsContext& graphicsContext,
		const ColorInfo& colorInfo,
		Size inputSize,
		Size rendererSize
	) noexcept;

	HRESULT Draw(
		uint32_t frameIndex,
		ID3D12DescriptorHeap* heap,
		D3D12_GPU_DESCRIPTOR_HANDLE heapGpuHandle,
		uint32_t inputSrvIdx,
		uint32_t outputUavIdx
	) noexcept;

	Size GetOutputSize() const noexcept {
		return _outputSize;
	}

	void OnResized(Size rendererSize) noexcept;

	void OnColorInfoChanged(const ColorInfo& colorInfo) noexcept;

private:
	GraphicsContext* _graphicsContext = nullptr;
	ColorInfo _colorInfo;

	Size _inputSize{};
	Size _outputSize{};

#ifdef MP_ENABLE_RTX_TRUE_HDR
	std::optional<RtxTrueHdrDrawer> _rtxTrueHdrDrawer;
	winrt::com_ptr<ID3D12Resource> _rtxTrueHdrOutput;
	// UAV + SRV
	uint32_t _rtxTrueHdrOutputDescriptorBaseIdx = std::numeric_limits<uint32_t>::max();
#endif
	std::optional<CatmullRomDrawer> _catmullRomDrawer;

	winrt::com_ptr<ID3D12QueryHeap> _queryHeap;
	winrt::com_ptr<ID3D12Resource> _queryResultBuffer;
	UINT64 _timestampFrequency = 0;

	bool _isFP16Supported = false;
};

}

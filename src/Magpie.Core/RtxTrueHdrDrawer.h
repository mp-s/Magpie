#pragma once

#ifdef MP_ENABLE_RTX_TRUE_HDR

struct NVSDK_NGX_Parameter;
struct NVSDK_NGX_Handle;

namespace Magpie {

class GraphicsContext;

class RtxTrueHdrDrawer {
public:
	~RtxTrueHdrDrawer() noexcept;

	HRESULT Initialize(
		GraphicsContext& graphicsContext,
		Size inputSize,
		const ColorInfo& colorInfo
	) noexcept;

	Size GetOutputSize() const noexcept {
		return _inputSize;
	}

	HRESULT Draw(uint32_t inputSrvOffset, uint32_t outputUavOffset) noexcept;

	void OnColorInfoChanged(const ColorInfo& colorInfo) noexcept;

private:
	HRESULT _InitializePSO() noexcept;

	GraphicsContext* _graphicsContext = nullptr;

	Size _inputSize{};
	ColorInfo _colorInfo;

	winrt::com_ptr<ID3D12Resource> _ngxInputResource;
	winrt::com_ptr<ID3D12Resource> _ngxOutputResource;

	uint32_t _descriptorBaseOffset = std::numeric_limits<uint32_t>::max();

	NVSDK_NGX_Parameter* _ngxParameters = nullptr;
	NVSDK_NGX_Handle* _trueHdrFeature = nullptr;

	winrt::com_ptr<ID3D12RootSignature> _preRootSignature;
	winrt::com_ptr<ID3D12RootSignature> _postRootSignature;
	winrt::com_ptr<ID3D12PipelineState> _prePSO;
	winrt::com_ptr<ID3D12PipelineState> _postPSO;

	bool _isNgxInitialized = false;
};

}

#endif

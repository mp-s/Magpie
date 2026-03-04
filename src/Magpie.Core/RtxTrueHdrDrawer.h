#pragma once

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

	HRESULT Draw() noexcept;

	void OnColorInfoChanged(const ColorInfo& colorInfo) noexcept;

private:
	GraphicsContext* _graphicsContext = nullptr;

	Size _inputSize{};
	ColorInfo _colorInfo;

	winrt::com_ptr<ID3D12Resource> _ngxInputResource;
	winrt::com_ptr<ID3D12Resource> _ngxOutputResource;
	winrt::com_ptr<ID3D12Resource> _colorFactorResource;

	NVSDK_NGX_Parameter* _ngxParameters = nullptr;
	NVSDK_NGX_Handle* _trueHdrFeature = nullptr;
	bool _isNgxInitialized = false;
};

}

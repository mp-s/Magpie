#pragma once
#include "EffectDrawerBase.h"

namespace Magpie {

class GraphicsContext;

class CatumullRomEffectDrawer : public EffectDrawerBase {
public:
	virtual ~CatumullRomEffectDrawer() noexcept = default;

	HRESULT Initialize(GraphicsContext& graphicContext) noexcept;

private:
	winrt::com_ptr<ID3D12RootSignature> _rootSignature;
	winrt::com_ptr<ID3D12PipelineState> _pipelineState;
};

}

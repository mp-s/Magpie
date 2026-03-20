#pragma once
#include "EffectDrawerBase.h"

namespace Magpie {

class ShaderEffectDrawer : public EffectDrawerBase {
public:
	virtual ~ShaderEffectDrawer() noexcept = default;

	bool Initialize(
		D3D12Context& d3d12Context,
		const EffectOption& effectOption
	) noexcept override;

	bool Bind(
		ID3D12Resource* inputResource,
		Size inputSize,
		const ColorInfo& colorInfo,
		Size& outputSize
	) noexcept override;

	HRESULT Draw(
		ComputeContext& computeContext,
		uint32_t inputSrvOffset,
		uint32_t outputUavOffset
	) noexcept override;
};

}

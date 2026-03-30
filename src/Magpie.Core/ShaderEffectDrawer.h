#pragma once
#include "EffectDrawerBase.h"

namespace Magpie {

class ShaderEffectDrawer : public EffectDrawerBase {
public:
	virtual ~ShaderEffectDrawer() noexcept = default;

	const EffectInfo* Initialize(
		D3D12Context& d3d12Context,
		const EffectOption& effectOption
	) noexcept override;

	bool Bind(SizeU inputSize, const ColorInfo& colorInfo) noexcept override;

	bool IsReady() noexcept override;

	HRESULT Draw(
		ComputeContext& computeContext,
		uint32_t inputSrvOffset,
		uint32_t outputUavOffset
	) noexcept override;

private:
	D3D12Context* _d3d12Context = nullptr;
	const EffectOption* _effectOption = nullptr;
	std::string _compilationTaskId;
};

}

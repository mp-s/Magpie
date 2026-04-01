#pragma once
#include "EffectDrawerBase.h"

namespace Magpie {

struct ShaderEffectDrawInfo;

class ShaderEffectDrawer : public EffectDrawerBase {
public:
	virtual ~ShaderEffectDrawer() noexcept;

	const EffectInfo* Initialize(
		D3D12Context& d3d12Context,
		const EffectOption& effectOption
	) noexcept override;

	bool Bind(SizeU inputSize, const ColorInfo& colorInfo) noexcept override;

	EffectDrawerState GetState() noexcept override;

	HRESULT Draw(
		ComputeContext& computeContext,
		uint32_t inputSrvOffset,
		uint32_t outputUavOffset
	) noexcept override;

private:
	D3D12Context* _d3d12Context = nullptr;
	const EffectOption* _effectOption = nullptr;

	std::string _compilationTaskId;
	const ShaderEffectDrawInfo* _drawInfo;
};

}

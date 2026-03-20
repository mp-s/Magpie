#pragma once

namespace Magpie {

class D3D12Context;
struct EffectOption;
class ComputeContext;

class EffectDrawerBase {
public:
	EffectDrawerBase() = default;
	EffectDrawerBase(const EffectDrawerBase&) = delete;
	EffectDrawerBase(EffectDrawerBase&&) = delete;

	virtual ~EffectDrawerBase() noexcept = default;

	virtual bool Initialize(
		D3D12Context& d3d12Context,
		const EffectOption& effectOption
	) noexcept = 0;

	virtual bool Bind(
		ID3D12Resource* inputResource,
		Size inputSize,
		const ColorInfo& colorInfo,
		Size& outputSize
	) noexcept = 0;

	virtual HRESULT Draw(
		ComputeContext& computeContext,
		uint32_t inputSrvOffset,
		uint32_t outputUavOffset
	) noexcept = 0;
};

}

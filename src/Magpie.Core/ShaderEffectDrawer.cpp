#include "pch.h"
#include "ShaderEffectDrawer.h"

namespace Magpie {

bool ShaderEffectDrawer::Initialize(
	D3D12Context& /*d3d12Context*/,
	const EffectOption& /*effectOption*/
) noexcept {
	return false;
}

bool ShaderEffectDrawer::Bind(
	ID3D12Resource* /*inputResource*/,
	Size /*inputSize*/,
	const ColorInfo& /*colorInfo*/,
	Size& /*outputSize*/
) noexcept {
	return false;
}

HRESULT ShaderEffectDrawer::Draw(
	ComputeContext& /*computeContext*/,
	uint32_t /*inputSrvOffset*/,
	uint32_t /*outputUavOffset*/
) noexcept {
	return E_NOTIMPL;
}

}

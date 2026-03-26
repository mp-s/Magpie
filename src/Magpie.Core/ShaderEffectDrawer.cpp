#include "pch.h"
#include "ShaderEffectDrawer.h"
#include "EffectsService.h"

namespace Magpie {

bool ShaderEffectDrawer::Initialize(
	D3D12Context& /*d3d12Context*/,
	const EffectOption& /*effectOption*/
) noexcept {
	return false;
}

bool ShaderEffectDrawer::Bind(
	ID3D12Resource* /*inputResource*/,
	SizeU /*inputSize*/,
	const ColorInfo& /*colorInfo*/,
	SizeU& /*outputSize*/
) noexcept {
	return false;
}

bool ShaderEffectDrawer::IsReady() noexcept {
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

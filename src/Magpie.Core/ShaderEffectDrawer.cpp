#include "pch.h"
#include "D3D12Context.h"
#include "EffectsService.h"
#include "Logger.h"
#include "ScalingOptions.h"
#include "ScalingWindow.h"
#include "ShaderEffectDrawer.h"

namespace Magpie {

const EffectInfo* ShaderEffectDrawer::Initialize(
	D3D12Context& d3d12Context,
	const EffectOption& effectOption
) noexcept {
	_d3d12Context = &d3d12Context;
	_effectOption = &effectOption;

	const EffectInfo* effectInfo = EffectsService::Get().GetEffect(effectOption.name);
	if (!effectInfo) {
		Logger::Get().Error("EffectsService::GetEffect 失败");
		return nullptr;
	}

	return effectInfo;
}

bool ShaderEffectDrawer::Bind(SizeU /*inputSize*/, const ColorInfo& colorInfo) noexcept {
	const ScalingOptions& options = ScalingWindow::Get().Options();

	_compilationTaskId = EffectsService::Get().SubmitCompileShaderEffectTask(
		_effectOption->name,
		options.IsInlineParams() ? &_effectOption->parameters : nullptr,
		_d3d12Context->GetShaderModel(),
		_d3d12Context->IsFP16Supported(),
		colorInfo.kind != winrt::AdvancedColorKind::StandardDynamicRange
	);
	if (_compilationTaskId.empty()) {
		Logger::Get().Error("EffectsService::SubmitCompileShaderEffectTask 失败");
		return false;
	}

	return true;
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

#pragma once
#include "EffectDrawerBase.h"
#include "SmallVector.h"

namespace Magpie {

struct ShaderEffectDrawInfo;

class ShaderEffectDrawer : public EffectDrawerBase {
public:
	virtual ~ShaderEffectDrawer() noexcept;

	const EffectInfo* Initialize(
		D3D12Context& d3d12Context,
		const EffectOption& effectOption
	) noexcept override;

	void Bind(SizeU inputSize, SizeU outputSize, const ColorInfo& colorInfo) noexcept override;

	HRESULT Update(EffectDrawerState& state, std::string& message) noexcept override;

	HRESULT Draw(
		ComputeContext& computeContext,
		uint32_t inputSrvOffset,
		uint32_t outputUavOffset
	) noexcept override;

private:
	HRESULT _CreateDeviceResources();

	D3D12Context* _d3d12Context = nullptr;
	const EffectOption* _effectOption = nullptr;

	SizeU _inputSize{};
	SizeU _outputSize{};
	ColorInfo _colorInfo;
	std::string _compilationTaskId;
	const ShaderEffectDrawInfo* _drawInfo = nullptr;
	std::string _errorMsg;

	struct _PassData {
		winrt::com_ptr<ID3D12RootSignature> rootSignature;
		winrt::com_ptr<ID3D12PipelineState> pso;
		uint32_t descriptorBaseOffset = std::numeric_limits<uint32_t>::max();
		SizeU dispatchCount;
	};
	SmallVector<_PassData> _passDatas;
	SmallVector<winrt::com_ptr<ID3D12Resource>> _textures;
	uint32_t _descriptorCount = 0;
};

}

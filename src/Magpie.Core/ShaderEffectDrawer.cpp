#include "pch.h"
#include "D3D12Context.h"
#include "DescriptorHeap.h"
#include "EffectsService.h"
#include "Logger.h"
#include "ScalingOptions.h"
#include "ScalingWindow.h"
#include "ShaderEffectDrawer.h"
// Conan 的 muparser 不含 UNICODE 支持
#pragma push_macro("_UNICODE")
#undef _UNICODE
#include <muParser.h>
#pragma pop_macro("_UNICODE")

namespace Magpie {

ShaderEffectDrawer::~ShaderEffectDrawer() noexcept {
	if (!_passDatas.empty()) {
		uint32_t descriptorBaseOffset = _passDatas[0].descriptorBaseOffset;
		if (descriptorBaseOffset != std::numeric_limits<uint32_t>::max()) {
			_d3d12Context->GetDescriptorHeap().Free(descriptorBaseOffset, _descriptorCount);
		}
	}

	if (!_compilationTaskId.empty()) {
		EffectsService::Get().ReleaseTask(_compilationTaskId);
	}
}

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

void ShaderEffectDrawer::Bind(SizeU inputSize, SizeU outputSize, const ColorInfo& colorInfo) noexcept {
	if (!_errorMsg.empty()) {
		return;
	}

	_inputSize = inputSize;
	_outputSize = outputSize;
	bool wasSrgb = _colorInfo.kind != winrt::AdvancedColorKind::StandardDynamicRange;
	bool isSrgb = colorInfo.kind != winrt::AdvancedColorKind::StandardDynamicRange;
	_colorInfo = colorInfo;

	if (wasSrgb == isSrgb && !_compilationTaskId.empty()) {
		// TODO: 更新常量
		return;
	}

	if (!_compilationTaskId.empty()) {
		EffectsService::Get().ReleaseTask(_compilationTaskId);
		_drawInfo = nullptr;
	}

	const ScalingOptions& options = ScalingWindow::Get().Options();
	_compilationTaskId = EffectsService::Get().SubmitCompileShaderEffectTask(
		_effectOption->name,
		options.IsInlineParams() ? &_effectOption->parameters : nullptr,
		_d3d12Context->GetShaderModel(),
		_d3d12Context->IsFP16Supported(),
		colorInfo.kind != winrt::AdvancedColorKind::StandardDynamicRange,
		options.IsSaveEffectSources(),
		options.IsWarningsAreErrors()
	);
	if (_compilationTaskId.empty()) {
		_errorMsg = "编译失败";
		Logger::Get().Error("EffectsService::SubmitCompileShaderEffectTask 失败");
	}
}

HRESULT ShaderEffectDrawer::Update(EffectDrawerState& state, std::string& message) noexcept {
	if (_drawInfo) {
		state = EffectDrawerState::Ready;
		return S_OK;
	}

	if (!_errorMsg.empty()) {
		state = EffectDrawerState::Error;
		message = _errorMsg;
		return S_OK;
	}

	if (!EffectsService::Get().GetTaskResult(_compilationTaskId, &_drawInfo)) {
		state = EffectDrawerState::Error;
		return S_OK;
	}

	if (!_drawInfo) {
		state = EffectDrawerState::NotReady;
		return S_OK;
	}

	HRESULT hr = _CreateDeviceResources();
	if (FAILED(hr)) {
		Logger::Get().ComError("_CreateDeviceResources 失败", hr);
		return hr;
	}

	if (!_errorMsg.empty()) {
		_drawInfo = nullptr;
		state = EffectDrawerState::Error;
		return S_OK;
	}

	state = EffectDrawerState::Ready;
	return S_OK;
}

HRESULT ShaderEffectDrawer::Draw(
	ComputeContext& /*computeContext*/,
	uint32_t /*inputSrvOffset*/,
	uint32_t /*outputUavOffset*/
) noexcept {
	assert(_drawInfo);

	return E_NOTIMPL;
}

HRESULT ShaderEffectDrawer::_CreateDeviceResources() {
	ID3D12Device5* device = _d3d12Context->GetDevice();

	const uint32_t passCount = (uint32_t)_drawInfo->passes.size();
	_passDatas.resize(passCount);

	for (uint32_t passIdx = 0; passIdx < passCount; ++passIdx) {
		_PassData& curPassData = _passDatas[passIdx];
		const ShaderEffectPassDesc& curPassDesc = _drawInfo->passes[passIdx];
		
		winrt::com_ptr<ID3DBlob> signature;

		std::array<D3D12_ROOT_PARAMETER1, 4> rootParams{};
		uint32_t curRootParamIdx = 0;
		std::array<D3D12_DESCRIPTOR_RANGE1, 4> descriptorRanges{};
		uint32_t curDescriptorRangeIdx = 0;

		rootParams[curRootParamIdx++] = D3D12_ROOT_PARAMETER1{
			.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV,
			.Descriptor = {
				.ShaderRegister = 0,
				.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE
			}
		};

		// INPUT 和 OUTPUT 的描述符独立， 其他描述符连续
		std::span<const uint32_t> otherInputs;
		std::span<const uint32_t> otherOutputs;
		if (!curPassDesc.inputs.empty()) {
			if (curPassDesc.inputs[0] == 0) {
				descriptorRanges[curDescriptorRangeIdx] = CD3DX12_DESCRIPTOR_RANGE1(
					D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0,
					D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE);

				rootParams[curRootParamIdx++] = D3D12_ROOT_PARAMETER1{
					.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
					.DescriptorTable = {
						.NumDescriptorRanges = 1,
						.pDescriptorRanges = &descriptorRanges[curDescriptorRangeIdx]
					}
				};

				++curDescriptorRangeIdx;
				otherInputs = std::span(curPassDesc.inputs.begin() + 1, curPassDesc.inputs.end());
			} else {
				otherInputs = curPassDesc.inputs;
			}
		}

		assert(!curPassDesc.outputs.empty());
		if (curPassDesc.outputs[0] == 1) {
			descriptorRanges[curDescriptorRangeIdx] = CD3DX12_DESCRIPTOR_RANGE1(
				D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE);

			rootParams[curRootParamIdx++] = D3D12_ROOT_PARAMETER1{
				.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
				.DescriptorTable = {
					.NumDescriptorRanges = 1,
					.pDescriptorRanges = &descriptorRanges[curDescriptorRangeIdx]
				}
			};

			++curDescriptorRangeIdx;
			otherOutputs = std::span(curPassDesc.outputs.begin() + 1, curPassDesc.outputs.end());
		} else {
			otherOutputs = curPassDesc.outputs;
		}
		
		if (!otherInputs.empty() || !otherOutputs.empty()) {
			const uint32_t startIdx = curDescriptorRangeIdx;

			if (!otherInputs.empty()) {
				descriptorRanges[curDescriptorRangeIdx++] = CD3DX12_DESCRIPTOR_RANGE1(
					D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
					(UINT)otherInputs.size(),
					UINT(otherInputs.data() != curPassDesc.inputs.data()),
					0,
					D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE
				);
			}

			if (!otherOutputs.empty()) {
				descriptorRanges[curDescriptorRangeIdx++] = CD3DX12_DESCRIPTOR_RANGE1(
					D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
					(UINT)otherOutputs.size(),
					UINT(otherOutputs.data() != curPassDesc.outputs.data()),
					0,
					D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE
				);
			}

			rootParams[curRootParamIdx++] = D3D12_ROOT_PARAMETER1{
				.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
				.DescriptorTable = {
					.NumDescriptorRanges = curDescriptorRangeIdx - startIdx,
					.pDescriptorRanges = &descriptorRanges[startIdx]
				}
			};
		}

		SmallVector<D3D12_STATIC_SAMPLER_DESC> samplerDescs(_drawInfo->samplers.size());
		for (size_t i = 0; i < samplerDescs.size(); ++i) {
			const ShaderEffectSamplerDesc& samplerDesc = _drawInfo->samplers[i];

			D3D12_TEXTURE_ADDRESS_MODE addressMode =
				samplerDesc.addressType == ShaderEffectSamplerAddressType::Clamp ?
				D3D12_TEXTURE_ADDRESS_MODE_CLAMP : D3D12_TEXTURE_ADDRESS_MODE_WRAP;

			samplerDescs[i] = D3D12_STATIC_SAMPLER_DESC{
				.Filter = samplerDesc.filterType == ShaderEffectSamplerFilterType::Point ?
					D3D12_FILTER_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_LINEAR,
				.AddressU = addressMode,
				.AddressV = addressMode,
				.AddressW = addressMode,
				.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER,
				.ShaderRegister = (UINT)i
			};
		}

		CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc(
			(UINT)curRootParamIdx, rootParams.data(),
			(UINT)samplerDescs.size(), samplerDescs.data());

		HRESULT hr = D3DX12SerializeVersionedRootSignature(
			&rootSignatureDesc,
			_d3d12Context->GetRootSignatureVersion(),
			signature.put(),
			nullptr
		);
		if (FAILED(hr)) {
			Logger::Get().ComError("D3DX12SerializeVersionedRootSignature 失败", hr);
			return hr;
		}

		hr = device->CreateRootSignature(
			0,
			signature->GetBufferPointer(),
			signature->GetBufferSize(),
			IID_PPV_ARGS(&curPassData.rootSignature)
		);
		if (FAILED(hr)) {
			Logger::Get().ComError("CreateRootSignature 失败", hr);
			return hr;
		}

		D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {
			.pRootSignature = curPassData.rootSignature.get(),
			.CS = CD3DX12_SHADER_BYTECODE(curPassDesc.byteCode.get())
		};
		hr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&curPassData.pso));
		if (FAILED(hr)) {
			Logger::Get().ComError("CreateComputePipelineState 失败", hr);
			return hr;
		}
	}

	// 需要的描述符数量不会变化
	if (_descriptorCount == 0) {
		// TODO: 合并输入和输出相同的通道
		for (uint32_t passIdx = 0; passIdx < passCount; ++passIdx) {
			_PassData& curPassData = _passDatas[passIdx];
			const ShaderEffectPassDesc& curPassDesc = _drawInfo->passes[passIdx];

			// 暂时保存偏移
			curPassData.descriptorBaseOffset = _descriptorCount;

			_descriptorCount += (uint32_t)curPassDesc.inputs.size();
			_descriptorCount += (uint32_t)curPassDesc.outputs.size();

			if (!curPassDesc.inputs.empty() && curPassDesc.inputs[0] == 0) {
				--_descriptorCount;
			}
			if (curPassDesc.outputs[0] == 1) {
				--_descriptorCount;
			}
		}
		
		uint32_t descriptorBaseOffset;
		HRESULT hr = _d3d12Context->GetDescriptorHeap()
			.Alloc(_descriptorCount, descriptorBaseOffset);
		if (FAILED(hr)) {
			Logger::Get().ComError("DescriptorHeap::Alloc 失败", hr);
			return hr;
		}

		for (_PassData& passData : _passDatas) {
			passData.descriptorBaseOffset += descriptorBaseOffset;
		}
	}

	{
		_textures.resize(_drawInfo->textures.size());

		mu::Parser exprParser;
		exprParser.DefineConst("INPUT_WIDTH", _inputSize.width);
		exprParser.DefineConst("INPUT_HEIGHT", _inputSize.height);
		exprParser.DefineConst("OUTPUT_WIDTH", _outputSize.width);
		exprParser.DefineConst("OUTPUT_HEIGHT", _outputSize.height);

		for (size_t i = 0; i < _textures.size(); ++i) {
			const ShaderEffectTextureDesc& effectTexDesc = _drawInfo->textures[i];

			DXGI_FORMAT dxgiFormat;

			if (effectTexDesc.source.empty()) {
				SizeU texSize{};
				try {
					exprParser.SetExpr(effectTexDesc.widthExpr);
					long width = std::lround(exprParser.Eval());
					exprParser.SetExpr(effectTexDesc.heightExpr);
					long height = std::lround(exprParser.Eval());

					if (width > 0 && height > 0) {
						texSize.width = (uint32_t)width;
						texSize.height = (uint32_t)height;
					}
				} catch (const mu::ParserError& e) {
					Logger::Get().Error(fmt::format("计算纹理 {} 尺寸失败: {}",
						effectTexDesc.name, e.GetMsg()));
				}

				if (texSize.width == 0) {
					_errorMsg = fmt::format("计算纹理 {} 尺寸失败", effectTexDesc.name);
					return S_OK;
				}

				if (effectTexDesc.format == ShaderEffectTextureFormat::COLOR_SPACE_ADAPTIVE) {
					if (_colorInfo.kind == winrt::AdvancedColorKind::StandardDynamicRange) {
						dxgiFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
					} else {
						dxgiFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
					}
				} else {
					dxgiFormat = SHADER_TEXTURE_FORMAT_PROPS[(uint32_t)effectTexDesc.format].dxgiFormat;
				}

				if (_textures[i]) {
					D3D12_RESOURCE_DESC texDesc = _textures[i]->GetDesc();
					if (texDesc.Width == texSize.width && texDesc.Height == texSize.height &&
						texDesc.Format == dxgiFormat) {
						continue;
					}
				}

				CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);

				D3D12_HEAP_FLAGS heapFlags = _d3d12Context->IsHeapFlagCreateNotZeroedSupported() ?
					D3D12_HEAP_FLAG_CREATE_NOT_ZEROED : D3D12_HEAP_FLAG_NONE;

				CD3DX12_RESOURCE_DESC texDesc = CD3DX12_RESOURCE_DESC::Tex2D(dxgiFormat,
					texSize.width, texSize.height, 1, 1, 1, 0,
					D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
				);

				HRESULT hr = device->CreateCommittedResource(&heapProps, heapFlags, &texDesc,
					D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&_textures[i]));
				if (FAILED(hr)) {
					Logger::Get().ComError("CreateCommittedResource 失败", hr);
					return hr;
				}
			} else {
				if (_textures[i]) {
					continue;
				}

				// TODO
			}

			auto& descriptorHeap = _d3d12Context->GetDescriptorHeap();
			const uint32_t descriptorSize = descriptorHeap.GetDescriptorSize();

			for (uint32_t passIdx = 0; passIdx < passCount; ++passIdx) {
				const ShaderEffectPassDesc& curPassDesc = _drawInfo->passes[passIdx];
				
				CD3DX12_CPU_DESCRIPTOR_HANDLE descriptorCpuHandle(
					descriptorHeap.GetCpuHandle(_passDatas[passIdx].descriptorBaseOffset));

				CD3DX12_SHADER_RESOURCE_VIEW_DESC srvDesc =
					CD3DX12_SHADER_RESOURCE_VIEW_DESC::Tex2D(dxgiFormat, 1);

				if (!curPassDesc.inputs.empty()) {
					auto it = std::find(curPassDesc.inputs.begin(), curPassDesc.inputs.end(), i + 2);
					if (it != curPassDesc.inputs.end()) {
						size_t offset = it - curPassDesc.inputs.begin();
						if (curPassDesc.inputs[0] == 0) {
							--offset;
						}
						device->CreateShaderResourceView(_textures[i].get(), &srvDesc,
							CD3DX12_CPU_DESCRIPTOR_HANDLE(descriptorCpuHandle, (INT)offset, descriptorSize));
					}

					descriptorCpuHandle.Offset(
						curPassDesc.inputs[0] == 0 ? curPassDesc.inputs.size() - 1 : curPassDesc.inputs.size(),
						descriptorSize
					);
				}

				CD3DX12_UNORDERED_ACCESS_VIEW_DESC uavDesc =
					CD3DX12_UNORDERED_ACCESS_VIEW_DESC::Tex2D(dxgiFormat);

				auto it = std::find(curPassDesc.outputs.begin(), curPassDesc.outputs.end(), i + 2);
				if (it != curPassDesc.outputs.end()) {
					size_t offset = it - curPassDesc.outputs.begin();
					if (curPassDesc.outputs[0] == 1) {
						--offset;
					}
					device->CreateUnorderedAccessView(_textures[i].get(), nullptr, &uavDesc,
						CD3DX12_CPU_DESCRIPTOR_HANDLE(descriptorCpuHandle, (INT)offset, descriptorSize));
				}
			}
		}
	}

	for (uint32_t passIdx = 0; passIdx < passCount; ++passIdx) {
		const ShaderEffectPassDesc& curPassDesc = _drawInfo->passes[passIdx];

		SizeU outputSize;
		if (curPassDesc.outputs[0] == 1) {
			outputSize = _outputSize;
		} else {
			D3D12_RESOURCE_DESC texDesc = _textures[size_t(curPassDesc.outputs[0] - 2)]->GetDesc();
			outputSize = { (uint32_t)texDesc.Width,(uint32_t)texDesc.Height };
		}

		_passDatas[passIdx].dispatchCount = {
			(outputSize.width + curPassDesc.blockSize.width - 1) / curPassDesc.blockSize.width,
			(outputSize.height + curPassDesc.blockSize.height - 1) / curPassDesc.blockSize.height
		};
	}

	return S_OK;
}

}

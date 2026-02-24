#include "pch.h"
#include "CatmullRomDrawer.h"
#include "DirectXHelper.h"
#include "GraphicsContext.h"
#include "Logger.h"
#include "shaders/CatmullRomCS.h"
#include "shaders/CatmullRomCS_sRGB.h"

namespace Magpie {

HRESULT CatmullRomDrawer::Initialize(GraphicsContext& graphicsContext) noexcept {
	_graphicsContext = &graphicsContext;

	winrt::com_ptr<ID3DBlob> signature;

	CD3DX12_DESCRIPTOR_RANGE1 srvRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0,
		D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE);
	CD3DX12_DESCRIPTOR_RANGE1 uavRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0,
		D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE);

	D3D12_ROOT_PARAMETER1 rootParams[] = {
		{
			.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
			.Constants = {
				.Num32BitValues = 8
			}
		},
		{
			.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
			.DescriptorTable = {
				.NumDescriptorRanges = 1,
				.pDescriptorRanges = &srvRange
			}
		},
		{
			.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
			.DescriptorTable = {
				.NumDescriptorRanges = 1,
				.pDescriptorRanges = &uavRange
			}
		}
	};

	D3D12_STATIC_SAMPLER_DESC samplerDesc = {
		.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR,
		.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
		.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
		.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
		.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER,
		.ShaderRegister = 0
	};
	
	CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc(
		(UINT)std::size(rootParams), rootParams, 1, &samplerDesc);

	HRESULT hr = D3DX12SerializeVersionedRootSignature(
		&rootSignatureDesc, graphicsContext.GetRootSignatureVersion(), signature.put(), nullptr);
	if (FAILED(hr)) {
		Logger::Get().ComError("D3DX12SerializeVersionedRootSignature 失败", hr);
		return hr;
	}

	hr = graphicsContext.GetDevice()->CreateRootSignature(
		0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&_rootSignature));
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateRootSignature 失败", hr);
		return hr;
	}

	return S_OK;
}

HRESULT CatmullRomDrawer::Draw(
	Size inputSize,
	Size outputSize,
	D3D12_GPU_DESCRIPTOR_HANDLE inputGpuHandle,
	D3D12_GPU_DESCRIPTOR_HANDLE outputGpuHandle,
	bool outputSrgb
) noexcept {
	ID3D12GraphicsCommandList* commandList = _graphicsContext->GetCommandList();

	if (outputSrgb) {
		if (!_srgbPipelineState) {
			D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {
				.pRootSignature = _rootSignature.get(),
				.CS = CD3DX12_SHADER_BYTECODE(CatmullRomCS_sRGB, sizeof(CatmullRomCS_sRGB))
			};
			HRESULT hr = _graphicsContext->GetDevice()->CreateComputePipelineState(
				&psoDesc, IID_PPV_ARGS(&_srgbPipelineState));
			if (FAILED(hr)) {
				Logger::Get().ComError("CreateComputePipelineState 失败", hr);
				return hr;
			}
		}
		
		commandList->SetPipelineState(_srgbPipelineState.get());
	} else {
		if (!_linearPipelineState) {
			D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {
				.pRootSignature = _rootSignature.get(),
				.CS = CD3DX12_SHADER_BYTECODE(CatmullRomCS, sizeof(CatmullRomCS))
			};
			HRESULT hr = _graphicsContext->GetDevice()->CreateComputePipelineState(
				&psoDesc, IID_PPV_ARGS(&_linearPipelineState));
			if (FAILED(hr)) {
				Logger::Get().ComError("CreateComputePipelineState 失败", hr);
				return hr;
			}
		}

		commandList->SetPipelineState(_linearPipelineState.get());
	}

	commandList->SetComputeRootSignature(_rootSignature.get());

	{
		DirectXHelper::Constant32 constants[] = {
			{.uintVal = inputSize.width},
			{.uintVal = inputSize.height},
			{.uintVal = outputSize.width},
			{.uintVal = outputSize.height},
			{.floatVal = 1.0f / inputSize.width},
			{.floatVal = 1.0f / inputSize.height},
			{.floatVal = 1.0f / outputSize.width},
			{.floatVal = 1.0f / outputSize.height}
		};
		commandList->SetComputeRoot32BitConstants(0, (UINT)std::size(constants), constants, 0);
	}

	commandList->SetComputeRootDescriptorTable(1, inputGpuHandle);
	commandList->SetComputeRootDescriptorTable(2, outputGpuHandle);

	constexpr std::pair<uint32_t, uint32_t> BLOCK_SIZE = { 16, 8 };
	commandList->Dispatch(
		(outputSize.width + BLOCK_SIZE.first - 1) / BLOCK_SIZE.first,
		(outputSize.height + BLOCK_SIZE.second - 1) / BLOCK_SIZE.second,
		1
	);

	return S_OK;
}

}

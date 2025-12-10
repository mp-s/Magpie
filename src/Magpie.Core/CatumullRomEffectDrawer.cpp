#include "pch.h"
#include "CatumullRomEffectDrawer.h"
#include "GraphicsContext.h"
#include "Logger.h"
#include "shaders/CatmullRomCS.h"

namespace Magpie {

HRESULT CatumullRomEffectDrawer::Initialize(GraphicsContext& graphicContext) noexcept {
	ID3D12Device5* device = graphicContext.GetDevice();

	{
		winrt::com_ptr<ID3DBlob> signature;

		CD3DX12_DESCRIPTOR_RANGE1 srvRange(
			D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE);
		CD3DX12_DESCRIPTOR_RANGE1 uavRange(
			D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE);

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
			.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR,
			.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
			.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
			.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
			.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER,
			.ShaderRegister = 0
		};
		
		CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc(
			(UINT)std::size(rootParams), rootParams, 1, &samplerDesc);

		HRESULT hr = D3DX12SerializeVersionedRootSignature(
			&rootSignatureDesc, graphicContext.GetRootSignatureVersion(), signature.put(), nullptr);
		if (FAILED(hr)) {
			Logger::Get().ComError("D3DX12SerializeVersionedRootSignature 失败", hr);
			return hr;
		}

		hr = device->CreateRootSignature(
			0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&_rootSignature));
		if (FAILED(hr)) {
			Logger::Get().ComError("CreateRootSignature 失败", hr);
			return hr;
		}
	}

	{
		D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {
			.pRootSignature = _rootSignature.get(),
			.CS = {.pShaderBytecode = CatmullRomCS, .BytecodeLength = sizeof(CatmullRomCS)}
		};
		HRESULT hr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&_pipelineState));
		if (FAILED(hr)) {
			Logger::Get().ComError("CreateComputePipelineState 失败", hr);
			return hr;
		}
	}
	
	return S_OK;
}

}

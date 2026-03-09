#include "pch.h"
#include "CatmullRomDrawer.h"
#include "DirectXHelper.h"
#include "GraphicsContext.h"
#include "DescriptorHeap.h"
#include "Logger.h"
#include "shaders/CatmullRomCS.h"
#include "shaders/CatmullRomCS_sRGB.h"
#include "shaders/CopyCS.h"
#include "shaders/CopyCS_sRGB.h"

namespace Magpie {

void CatmullRomDrawer::Initialize(GraphicsContext& graphicsContext) noexcept {
	_graphicsContext = &graphicsContext;
}

HRESULT CatmullRomDrawer::Draw(
	Size inputSize,
	Size outputSize,
	uint32_t inputSrvOffset,
	uint32_t outputUavOffset,
	bool outputSrgb
) noexcept {
	ID3D12GraphicsCommandList* commandList = _graphicsContext->GetCommandList();
	auto& descriptorHeap = _graphicsContext->GetDescriptorHeap();

	// 作为性能优化，输入和输出尺寸相同时原样复制
	if (inputSize == outputSize) {
		if (!_copyRootSignature) {
			HRESULT hr = _InitializeCopyRootSignature();
			if (FAILED(hr)) {
				Logger::Get().ComError("_InitializeCopyRootSignature 失败", hr);
				return hr;
			}
		}
		commandList->SetComputeRootSignature(_copyRootSignature.get());

		if (outputSrgb) {
			if (!_copySrgbPSO) {
				D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {
					.pRootSignature = _copyRootSignature.get(),
					.CS = CD3DX12_SHADER_BYTECODE(CopyCS_sRGB, sizeof(CopyCS_sRGB))
				};
				HRESULT hr = _graphicsContext->GetDevice()->CreateComputePipelineState(
					&psoDesc, IID_PPV_ARGS(&_copySrgbPSO));
				if (FAILED(hr)) {
					Logger::Get().ComError("CreateComputePipelineState 失败", hr);
					return hr;
				}
			}

			commandList->SetPipelineState(_copySrgbPSO.get());
		} else {
			if (!_copyPSO) {
				D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {
					.pRootSignature = _copyRootSignature.get(),
					.CS = CD3DX12_SHADER_BYTECODE(CopyCS, sizeof(CopyCS))
				};
				HRESULT hr = _graphicsContext->GetDevice()->CreateComputePipelineState(
					&psoDesc, IID_PPV_ARGS(&_copyPSO));
				if (FAILED(hr)) {
					Logger::Get().ComError("CreateComputePipelineState 失败", hr);
					return hr;
				}
			}

			commandList->SetPipelineState(_copyPSO.get());
		}

		commandList->SetComputeRootDescriptorTable(0, descriptorHeap.GetGpuHandle(inputSrvOffset));
		commandList->SetComputeRootDescriptorTable(1, descriptorHeap.GetGpuHandle(outputUavOffset));
	} else {
		if (!_catmullRomRootSignature) {
			HRESULT hr = _InitializeCatmullRomRootSignature();
			if (FAILED(hr)) {
				Logger::Get().ComError("_InitializeCatmullRomRootSignature 失败", hr);
				return hr;
			}
		}
		commandList->SetComputeRootSignature(_catmullRomRootSignature.get());

		if (outputSrgb) {
			if (!_catmullRomSrgbPSO) {
				D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {
					.pRootSignature = _catmullRomRootSignature.get(),
					.CS = CD3DX12_SHADER_BYTECODE(CatmullRomCS_sRGB, sizeof(CatmullRomCS_sRGB))
				};
				HRESULT hr = _graphicsContext->GetDevice()->CreateComputePipelineState(
					&psoDesc, IID_PPV_ARGS(&_catmullRomSrgbPSO));
				if (FAILED(hr)) {
					Logger::Get().ComError("CreateComputePipelineState 失败", hr);
					return hr;
				}
			}

			commandList->SetPipelineState(_catmullRomSrgbPSO.get());
		} else {
			if (!_catmullRomPSO) {
				D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {
					.pRootSignature = _catmullRomRootSignature.get(),
					.CS = CD3DX12_SHADER_BYTECODE(CatmullRomCS, sizeof(CatmullRomCS))
				};
				HRESULT hr = _graphicsContext->GetDevice()->CreateComputePipelineState(
					&psoDesc, IID_PPV_ARGS(&_catmullRomPSO));
				if (FAILED(hr)) {
					Logger::Get().ComError("CreateComputePipelineState 失败", hr);
					return hr;
				}
			}

			commandList->SetPipelineState(_catmullRomPSO.get());
		}

		DirectXHelper::Constant32 constants[] = {
			{.uintVal = inputSize.width},
			{.uintVal = inputSize.height},
			{.floatVal = 1.0f / inputSize.width},
			{.floatVal = 1.0f / inputSize.height},
			{.floatVal = 1.0f / outputSize.width},
			{.floatVal = 1.0f / outputSize.height}
		};
		commandList->SetComputeRoot32BitConstants(0, (UINT)std::size(constants), constants, 0);

		commandList->SetComputeRootDescriptorTable(1, descriptorHeap.GetGpuHandle(inputSrvOffset));
		commandList->SetComputeRootDescriptorTable(2, descriptorHeap.GetGpuHandle(outputUavOffset));
	}

	constexpr uint32_t BLOCK_SIZE = 16;
	commandList->Dispatch(
		(outputSize.width + BLOCK_SIZE - 1) / BLOCK_SIZE,
		(outputSize.height + BLOCK_SIZE - 1) / BLOCK_SIZE,
		1
	);

	return S_OK;
}

HRESULT CatmullRomDrawer::_InitializeCatmullRomRootSignature() noexcept {
	winrt::com_ptr<ID3DBlob> signature;

	CD3DX12_DESCRIPTOR_RANGE1 srvRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0,
		D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE);
	CD3DX12_DESCRIPTOR_RANGE1 uavRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0,
		D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE);

	D3D12_ROOT_PARAMETER1 rootParams[] = {
		{
			.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
			.Constants = {
				.Num32BitValues = 6
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
		&rootSignatureDesc,
		_graphicsContext->GetRootSignatureVersion(),
		signature.put(),
		nullptr
	);
	if (FAILED(hr)) {
		Logger::Get().ComError("D3DX12SerializeVersionedRootSignature 失败", hr);
		return hr;
	}

	hr = _graphicsContext->GetDevice()->CreateRootSignature(
		0,
		signature->GetBufferPointer(),
		signature->GetBufferSize(),
		IID_PPV_ARGS(&_catmullRomRootSignature)
	);
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateRootSignature 失败", hr);
		return hr;
	}

	return S_OK;
}

HRESULT CatmullRomDrawer::_InitializeCopyRootSignature() noexcept {
	winrt::com_ptr<ID3DBlob> signature;

	CD3DX12_DESCRIPTOR_RANGE1 srvRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0,
		D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE);
	CD3DX12_DESCRIPTOR_RANGE1 uavRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0,
		D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE);

	D3D12_ROOT_PARAMETER1 rootParams[] = {
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

	CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc(
		(UINT)std::size(rootParams), rootParams, 0, nullptr);

	HRESULT hr = D3DX12SerializeVersionedRootSignature(
		&rootSignatureDesc,
		_graphicsContext->GetRootSignatureVersion(),
		signature.put(),
		nullptr
	);
	if (FAILED(hr)) {
		Logger::Get().ComError("D3DX12SerializeVersionedRootSignature 失败", hr);
		return hr;
	}

	hr = _graphicsContext->GetDevice()->CreateRootSignature(
		0,
		signature->GetBufferPointer(),
		signature->GetBufferSize(),
		IID_PPV_ARGS(&_copyRootSignature)
	);
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateRootSignature 失败", hr);
		return hr;
	}

	return S_OK;
}

}

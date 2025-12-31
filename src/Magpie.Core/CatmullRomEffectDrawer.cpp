#include "pch.h"
#include "CatmullRomEffectDrawer.h"
#include "DirectXHelper.h"
#include "GraphicsContext.h"
#include "Logger.h"
#include "shaders/CatmullRomCS.h"
#include "shaders/CatmullRomCS_sRGB.h"

namespace Magpie {

uint32_t CatmullRomEffectDrawer::CalcDescriptorCount(bool isFirst, bool isLast) {
	return 2 - isFirst - isLast;
}

HRESULT CatmullRomEffectDrawer::Initialize(
	GraphicsContext& graphicsContext,
	Size inputSize,
	Size outputSize,
	EffectColorSpace inputColorSpace,
	EffectColorSpace outputColorSpace,
	CD3DX12_CPU_DESCRIPTOR_HANDLE& descriptorCpuHandle,
	CD3DX12_GPU_DESCRIPTOR_HANDLE& descriptorGpuHandle,
	uint32_t descriptorSize,
	ID3D12Resource* inputResource,
	ID3D12Resource* outputResource
) noexcept {
	// 第一个效果的输入色彩空间不会是 linear_sRGB
	assert(inputResource || inputColorSpace != EffectColorSpace::linear_sRGB);

	_graphicsContext = &graphicsContext;
	_inputSize = inputSize;
	_outputSize = outputSize;
	_inputColorSpace = inputColorSpace;
	_outputColorSpace = outputColorSpace;

	ID3D12Device5* device = graphicsContext.GetDevice();

	{
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
			.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
			.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
			.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
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
			.CS = CD3DX12_SHADER_BYTECODE(
				_outputColorSpace == EffectColorSpace::sRGB ? CatmullRomCS_sRGB : CatmullRomCS,
				_outputColorSpace == EffectColorSpace::sRGB ? sizeof(CatmullRomCS_sRGB) : sizeof(CatmullRomCS)
			)
		};
		HRESULT hr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&_pipelineState));
		if (FAILED(hr)) {
			Logger::Get().ComError("CreateComputePipelineState 失败", hr);
			return hr;
		}
	}

	_CreateDisplayDependentResources(
		descriptorCpuHandle, descriptorGpuHandle, descriptorSize, inputResource, outputResource);

	return S_OK;
}

void CatmullRomEffectDrawer::Draw(
	D3D12_GPU_DESCRIPTOR_HANDLE inputGpuHandle,
	D3D12_GPU_DESCRIPTOR_HANDLE outputGpuHandle
) noexcept {
	assert((bool)inputGpuHandle.ptr != (bool)_inputGpuHandle.ptr);
	assert((bool)outputGpuHandle.ptr != (bool)_outputGpuHandle.ptr);

	ID3D12GraphicsCommandList* commandList = _graphicsContext->GetCommandList();

	commandList->SetPipelineState(_pipelineState.get());
	commandList->SetComputeRootSignature(_rootSignature.get());

	{
		DirectXHelper::Constant32 constants[] = {
			{.uintVal = _inputSize.width},
			{.uintVal = _inputSize.height},
			{.uintVal = _outputSize.width},
			{.uintVal = _outputSize.height},
			{.floatVal = 1.0f / _inputSize.width},
			{.floatVal = 1.0f / _inputSize.height},
			{.floatVal = 1.0f / _outputSize.width},
			{.floatVal = 1.0f / _outputSize.height}
		};
		commandList->SetComputeRoot32BitConstants(0, (UINT)std::size(constants), constants, 0);
	}

	commandList->SetComputeRootDescriptorTable(
		1, inputGpuHandle.ptr ? inputGpuHandle : _inputGpuHandle);
	commandList->SetComputeRootDescriptorTable(
		2, outputGpuHandle.ptr ? outputGpuHandle : _outputGpuHandle);

	constexpr std::pair<uint32_t, uint32_t> BLOCK_SIZE = { 16, 8 };
	commandList->Dispatch(
		(_outputSize.width + BLOCK_SIZE.first - 1) / BLOCK_SIZE.first,
		(_outputSize.height + BLOCK_SIZE.second - 1) / BLOCK_SIZE.second,
		1
	);
}

void CatmullRomEffectDrawer::OnResized(
	Size inputSize,
	Size outputSize,
	CD3DX12_CPU_DESCRIPTOR_HANDLE& descriptorCpuHandle,
	CD3DX12_GPU_DESCRIPTOR_HANDLE& descriptorGpuHandle,
	uint32_t descriptorSize,
	ID3D12Resource* inputResource,
	ID3D12Resource* outputResource
) noexcept {
	_inputSize = inputSize;
	_outputSize = outputSize;

	_CreateDisplayDependentResources(
		descriptorCpuHandle, descriptorGpuHandle, descriptorSize, inputResource, outputResource);
}

void CatmullRomEffectDrawer::OnColorInfoChanged(
	EffectColorSpace inputColorSpace,
	EffectColorSpace outputColorSpace,
	CD3DX12_CPU_DESCRIPTOR_HANDLE& descriptorCpuHandle,
	CD3DX12_GPU_DESCRIPTOR_HANDLE& descriptorGpuHandle,
	uint32_t descriptorSize,
	ID3D12Resource* inputResource,
	ID3D12Resource* outputResource
) noexcept {
	if (_inputColorSpace == inputColorSpace && _outputColorSpace == outputColorSpace) {
		return;
	}
	_inputColorSpace = inputColorSpace;
	_outputColorSpace = outputColorSpace;

	_CreateDisplayDependentResources(
		descriptorCpuHandle, descriptorGpuHandle, descriptorSize, inputResource, outputResource);
}

void CatmullRomEffectDrawer::_CreateDisplayDependentResources(
	CD3DX12_CPU_DESCRIPTOR_HANDLE& descriptorCpuHandle,
	CD3DX12_GPU_DESCRIPTOR_HANDLE& descriptorGpuHandle,
	uint32_t descriptorSize,
	ID3D12Resource* inputResource,
	ID3D12Resource* outputResource
) noexcept {
	ID3D12Device5* device = _graphicsContext->GetDevice();

	if (inputResource) {
		DXGI_FORMAT format;
		if (_inputColorSpace == EffectColorSpace::linear_sRGB) {
			format = DXGI_FORMAT_R8G8B8A8_UNORM;
		} else if (_inputColorSpace == EffectColorSpace::sRGB) {
			format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		} else {
			format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		}

		CD3DX12_SHADER_RESOURCE_VIEW_DESC desc =
			CD3DX12_SHADER_RESOURCE_VIEW_DESC::Tex2D(format, 1);
		device->CreateShaderResourceView(inputResource, &desc, descriptorCpuHandle);

		_inputGpuHandle = descriptorGpuHandle;

		descriptorCpuHandle.Offset(1, descriptorSize);
		descriptorGpuHandle.Offset(1, descriptorSize);
	}

	if (outputResource) {
		DXGI_FORMAT format;
		if (_outputColorSpace == EffectColorSpace::scRGB) {
			format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		} else {
			format = DXGI_FORMAT_R8G8B8A8_UNORM;
		}

		CD3DX12_UNORDERED_ACCESS_VIEW_DESC desc =
			CD3DX12_UNORDERED_ACCESS_VIEW_DESC::Tex2D(format);
		device->CreateUnorderedAccessView(outputResource, nullptr, &desc, descriptorCpuHandle);

		_outputGpuHandle = descriptorGpuHandle;

		descriptorCpuHandle.Offset(1, descriptorSize);
		descriptorGpuHandle.Offset(1, descriptorSize);
	}
}

}

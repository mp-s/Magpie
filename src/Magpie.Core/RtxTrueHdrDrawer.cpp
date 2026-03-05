#include "pch.h"

#ifdef MP_ENABLE_RTX_TRUE_HDR

#include "RtxTrueHdrDrawer.h"
#include "GraphicsContext.h"
#include "DynamicDescriptorHeap.h"
#include "shaders/RtxTrueHdrPostCS.h"
#include "shaders/RtxTrueHdrPreCS.h"
#include "Logger.h"
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs_truehdr.h>

namespace Magpie {

RtxTrueHdrDrawer::~RtxTrueHdrDrawer() noexcept {
#ifdef _DEBUG
	if (_descriptorBaseIdx != std::numeric_limits<uint32_t>::max()) {
		_graphicsContext->GetDynamicDescriptorHeap().Free(_descriptorBaseIdx, 2);
	}
#endif

	if (_trueHdrFeature) {
		NVSDK_NGX_D3D12_ReleaseFeature(_trueHdrFeature);
	}
	if (_ngxParameters) {
		NVSDK_NGX_D3D12_DestroyParameters(_ngxParameters);
	}
	if (_isNgxInitialized) {
		NVSDK_NGX_D3D12_Shutdown1(_graphicsContext->GetDevice());
	}
}

HRESULT RtxTrueHdrDrawer::Initialize(
	GraphicsContext& graphicsContext,
	Size inputSize,
	const ColorInfo& colorInfo
) noexcept {
	_graphicsContext = &graphicsContext;
	_inputSize = inputSize;
	_colorInfo = colorInfo;

	NVSDK_NGX_Result status = NVSDK_NGX_D3D12_Init(0, L".", graphicsContext.GetDevice());
	if (NVSDK_NGX_FAILED(status)) {
		return E_FAIL;
	}

	_isNgxInitialized = true;

	status = NVSDK_NGX_D3D12_GetCapabilityParameters(&_ngxParameters);
	if (NVSDK_NGX_FAILED(status)) {
		return E_FAIL;
	}

	int available = 0;
	status = _ngxParameters->Get(NVSDK_NGX_Parameter_TrueHDR_Available, &available);
	if (NVSDK_NGX_FAILED(status) || !available) {
		return E_FAIL;
	}
	
	{
		ID3D12Device5* device = graphicsContext.GetDevice();

		CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
		D3D12_HEAP_FLAGS heapFlag = graphicsContext.IsHeapFlagCreateNotZeroedSupported() ?
			D3D12_HEAP_FLAG_CREATE_NOT_ZEROED : D3D12_HEAP_FLAG_NONE;
		// 使用 R10G10B10A2 格式以尽可能保留精度
		CD3DX12_RESOURCE_DESC texDesc = CD3DX12_RESOURCE_DESC::Tex2D(
			DXGI_FORMAT_R10G10B10A2_UNORM, inputSize.width, inputSize.height,
			1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		HRESULT hr = device->CreateCommittedResource(
			&heapProps, heapFlag, &texDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			nullptr, IID_PPV_ARGS(&_ngxInputResource));
		if (FAILED(hr)) {
			return hr;
		}

		texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		hr = device->CreateCommittedResource(
			&heapProps, heapFlag, &texDesc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			nullptr, IID_PPV_ARGS(&_ngxOutputResource));
		if (FAILED(hr)) {
			return hr;
		}
	}
	
	{
		auto& dynamicDescriptorHeap = graphicsContext.GetDynamicDescriptorHeap();

		HRESULT hr = dynamicDescriptorHeap.Alloc(2, _descriptorBaseIdx);
		if (FAILED(hr)) {
			return hr;
		}

		CD3DX12_UNORDERED_ACCESS_VIEW_DESC uavDesc = 
			CD3DX12_UNORDERED_ACCESS_VIEW_DESC::Tex2D(DXGI_FORMAT_R10G10B10A2_UNORM);
		dynamicDescriptorHeap.CreateUnorderedAccessView(
			_ngxInputResource.get(), &uavDesc, _descriptorBaseIdx);

		CD3DX12_SHADER_RESOURCE_VIEW_DESC srcDesc =
			CD3DX12_SHADER_RESOURCE_VIEW_DESC::Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT, 1);
		dynamicDescriptorHeap.CreateShaderResourceView(
			_ngxOutputResource.get(), &srcDesc, _descriptorBaseIdx + 1);
	}
	
	HRESULT hr = _InitializePSO();
	if (FAILED(hr)) {
		return hr;
	}

	return S_OK;
}

HRESULT RtxTrueHdrDrawer::Draw(
	ID3D12DescriptorHeap* heap,
	D3D12_GPU_DESCRIPTOR_HANDLE heapGpuHandle,
	uint32_t inputSrvIdx,
	uint32_t outputUavIdx
) noexcept {
	ID3D12GraphicsCommandList* commandList = _graphicsContext->GetCommandList();

	_graphicsContext->SetDescriptorHeap(heap);

	uint32_t descriptorSize = _graphicsContext->GetDynamicDescriptorHeap().GetDescriptorSize();

	commandList->SetPipelineState(_prePSO.get());
	commandList->SetComputeRootSignature(_preRootSignature.get());
	commandList->SetComputeRoot32BitConstants(0, 1, &_colorInfo.sdrWhiteLevel, 0);
	commandList->SetComputeRootDescriptorTable(
		1, CD3DX12_GPU_DESCRIPTOR_HANDLE(heapGpuHandle, inputSrvIdx, descriptorSize));
	commandList->SetComputeRootDescriptorTable(
		2, CD3DX12_GPU_DESCRIPTOR_HANDLE(heapGpuHandle, _descriptorBaseIdx, descriptorSize));

	constexpr uint32_t BLOCK_SIZE = 16;
	commandList->Dispatch(
		(_inputSize.width + BLOCK_SIZE - 1) / BLOCK_SIZE,
		(_inputSize.height + BLOCK_SIZE - 1) / BLOCK_SIZE,
		1
	);

	if (!_trueHdrFeature) {
		NVSDK_NGX_Result status = NVSDK_NGX_D3D12_CreateFeature(
			commandList, NVSDK_NGX_Feature_TrueHDR, _ngxParameters, &_trueHdrFeature);
		if (NVSDK_NGX_FAILED(status)) {
			return E_FAIL;
		}

		NVSDK_NGX_Parameter_SetD3d12Resource(
			_ngxParameters, NVSDK_NGX_Parameter_Input1, _ngxInputResource.get());
		NVSDK_NGX_Parameter_SetD3d12Resource(
			_ngxParameters, NVSDK_NGX_Parameter_Output, _ngxOutputResource.get());
		NVSDK_NGX_Parameter_SetUI(_ngxParameters, NVSDK_NGX_Parameter_TrueHDR_InLeft, 0);
		NVSDK_NGX_Parameter_SetUI(_ngxParameters, NVSDK_NGX_Parameter_TrueHDR_InTop, 0);
		NVSDK_NGX_Parameter_SetUI(_ngxParameters, NVSDK_NGX_Parameter_TrueHDR_InRight, _inputSize.width);
		NVSDK_NGX_Parameter_SetUI(_ngxParameters, NVSDK_NGX_Parameter_TrueHDR_InBottom, _inputSize.height);
		NVSDK_NGX_Parameter_SetUI(_ngxParameters, NVSDK_NGX_Parameter_TrueHDR_OutLeft, 0);
		NVSDK_NGX_Parameter_SetUI(_ngxParameters, NVSDK_NGX_Parameter_TrueHDR_OutTop, 0);
		NVSDK_NGX_Parameter_SetUI(_ngxParameters, NVSDK_NGX_Parameter_TrueHDR_OutRight, _inputSize.width);
		NVSDK_NGX_Parameter_SetUI(_ngxParameters, NVSDK_NGX_Parameter_TrueHDR_OutBottom, _inputSize.height);
		NVSDK_NGX_Parameter_SetUI(_ngxParameters, NVSDK_NGX_Parameter_TrueHDR_Contrast, 50);
		NVSDK_NGX_Parameter_SetUI(_ngxParameters, NVSDK_NGX_Parameter_TrueHDR_Saturation, 0);
		NVSDK_NGX_Parameter_SetUI(_ngxParameters, NVSDK_NGX_Parameter_TrueHDR_MiddleGray, 80);
		NVSDK_NGX_Parameter_SetUI(_ngxParameters, NVSDK_NGX_Parameter_TrueHDR_MaxLuminance,
			(uint32_t)std::lroundf(_colorInfo.maxLuminance * SCENE_REFERRED_SDR_WHITE_LEVEL));
	}

#ifdef _DEBUG
	// 忽略 NGX 产生的调试层警告
	winrt::com_ptr<ID3D12InfoQueue> infoQueue;
	_graphicsContext->GetDevice()->QueryInterface(IID_PPV_ARGS(&infoQueue));
	if (infoQueue) {
		D3D12_MESSAGE_ID denyList[] = {
			D3D12_MESSAGE_ID_CREATERESOURCE_STATE_IGNORED
		};
		D3D12_INFO_QUEUE_FILTER filter = {
			.DenyList = {
				.NumIDs = (UINT)std::size(denyList),
				.pIDList = denyList
			}
		};
		infoQueue->PushCopyOfStorageFilter();
		infoQueue->AddStorageFilterEntries(&filter);
	}
#endif

	NVSDK_NGX_Result status = NVSDK_NGX_D3D12_EvaluateFeature(
		commandList, _trueHdrFeature, _ngxParameters, nullptr);
	if (NVSDK_NGX_FAILED(status)) {
		return E_FAIL;
	}

#ifdef _DEBUG
	if (infoQueue) {
		infoQueue->PopStorageFilter();
	}
#endif

	_graphicsContext->InvalidateDescriptorHeapCache();
	_graphicsContext->SetDescriptorHeap(heap);

	commandList->SetPipelineState(_postPSO.get());
	commandList->SetComputeRootSignature(_postRootSignature.get());
	commandList->SetComputeRootDescriptorTable(
		0, CD3DX12_GPU_DESCRIPTOR_HANDLE(heapGpuHandle, _descriptorBaseIdx + 1, descriptorSize));
	commandList->SetComputeRootDescriptorTable(
		1, CD3DX12_GPU_DESCRIPTOR_HANDLE(heapGpuHandle, inputSrvIdx, descriptorSize));
	commandList->SetComputeRootDescriptorTable(
		2, CD3DX12_GPU_DESCRIPTOR_HANDLE(heapGpuHandle, outputUavIdx, descriptorSize));

	commandList->Dispatch(
		(_inputSize.width + BLOCK_SIZE - 1) / BLOCK_SIZE,
		(_inputSize.height + BLOCK_SIZE - 1) / BLOCK_SIZE,
		1
	);

	return S_OK;
}

void RtxTrueHdrDrawer::OnColorInfoChanged(const ColorInfo& colorInfo) noexcept {
	_colorInfo = colorInfo;

	if (_trueHdrFeature) {
		NVSDK_NGX_Parameter_SetUI(_ngxParameters, NVSDK_NGX_Parameter_TrueHDR_MaxLuminance,
			(uint32_t)std::lroundf(colorInfo.maxLuminance * SCENE_REFERRED_SDR_WHITE_LEVEL));
	}
}

HRESULT RtxTrueHdrDrawer::_InitializePSO() noexcept {
	ID3D12Device5* device = _graphicsContext->GetDevice();
	winrt::com_ptr<ID3DBlob> signature;

	{
		CD3DX12_DESCRIPTOR_RANGE1 srvRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0,
			D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE);
		CD3DX12_DESCRIPTOR_RANGE1 uavRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0,
			D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE);

		D3D12_ROOT_PARAMETER1 rootParams[] = {
			{
				.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
				.Constants = {
					.Num32BitValues = 1
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
	}
	
	HRESULT hr = device->CreateRootSignature(
		0,
		signature->GetBufferPointer(),
		signature->GetBufferSize(),
		IID_PPV_ARGS(&_preRootSignature)
	);
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateRootSignature 失败", hr);
		return hr;
	}

	{
		CD3DX12_DESCRIPTOR_RANGE1 srvRange1(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0,
			D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE);
		CD3DX12_DESCRIPTOR_RANGE1 srvRange2(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0,
			D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE);
		CD3DX12_DESCRIPTOR_RANGE1 uavRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0,
			D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE);

		D3D12_ROOT_PARAMETER1 rootParams[] = {
			{
				.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
				.DescriptorTable = {
					.NumDescriptorRanges = 1,
					.pDescriptorRanges = &srvRange1
				}
			},
			{
				.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
				.DescriptorTable = {
					.NumDescriptorRanges = 1,
					.pDescriptorRanges = &srvRange2
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

		hr = D3DX12SerializeVersionedRootSignature(
			&rootSignatureDesc,
			_graphicsContext->GetRootSignatureVersion(),
			signature.put(),
			nullptr
		);
		if (FAILED(hr)) {
			Logger::Get().ComError("D3DX12SerializeVersionedRootSignature 失败", hr);
			return hr;
		}

		hr = D3DX12SerializeVersionedRootSignature(
			&rootSignatureDesc,
			_graphicsContext->GetRootSignatureVersion(),
			signature.put(),
			nullptr
		);

		if (FAILED(hr)) {
			Logger::Get().ComError("D3DX12SerializeVersionedRootSignature 失败", hr);
			return hr;
		}
	}

	hr = device->CreateRootSignature(
		0,
		signature->GetBufferPointer(),
		signature->GetBufferSize(),
		IID_PPV_ARGS(&_postRootSignature)
	);
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateRootSignature 失败", hr);
		return hr;
	}

	D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {
		.pRootSignature = _preRootSignature.get(),
		.CS = CD3DX12_SHADER_BYTECODE(RtxTrueHdrPreCS, sizeof(RtxTrueHdrPreCS))
	};
	hr = _graphicsContext->GetDevice()->CreateComputePipelineState(
		&psoDesc, IID_PPV_ARGS(&_prePSO));
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateComputePipelineState 失败", hr);
		return hr;
	}

	psoDesc.pRootSignature = _postRootSignature.get();
	psoDesc.CS = CD3DX12_SHADER_BYTECODE(RtxTrueHdrPostCS, sizeof(RtxTrueHdrPostCS));
	hr = _graphicsContext->GetDevice()->CreateComputePipelineState(
		&psoDesc, IID_PPV_ARGS(&_postPSO));
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateComputePipelineState 失败", hr);
		return hr;
	}

	return S_OK;
}

}

#endif

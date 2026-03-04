#include "pch.h"
#include "RtxTrueHdrDrawer.h"
#include "GraphicsContext.h"
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs_truehdr.h>

namespace Magpie {

RtxTrueHdrDrawer::~RtxTrueHdrDrawer() noexcept {
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

	ID3D12Device5* device = graphicsContext.GetDevice();

	CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
	D3D12_HEAP_FLAGS heapFlag = _graphicsContext->IsHeapFlagCreateNotZeroedSupported() ?
		D3D12_HEAP_FLAG_CREATE_NOT_ZEROED : D3D12_HEAP_FLAG_NONE;
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
		&heapProps, heapFlag, &texDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		nullptr, IID_PPV_ARGS(&_ngxOutputResource));
	if (FAILED(hr)) {
		return hr;
	}

	hr = device->CreateCommittedResource(
		&heapProps, heapFlag, &texDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		nullptr, IID_PPV_ARGS(&_colorFactorResource));
	if (FAILED(hr)) {
		return hr;
	}

	return S_OK;
}

HRESULT RtxTrueHdrDrawer::Draw() noexcept {
	ID3D12GraphicsCommandList* commandList = _graphicsContext->GetCommandList();

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
		NVSDK_NGX_Parameter_SetUI(_ngxParameters, NVSDK_NGX_Parameter_TrueHDR_Contrast, 100);
		NVSDK_NGX_Parameter_SetUI(_ngxParameters, NVSDK_NGX_Parameter_TrueHDR_Saturation, 100);
		NVSDK_NGX_Parameter_SetUI(_ngxParameters, NVSDK_NGX_Parameter_TrueHDR_MiddleGray, 50);
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

	return S_OK;
}

void RtxTrueHdrDrawer::OnColorInfoChanged(const ColorInfo& colorInfo) noexcept {
	_colorInfo = colorInfo;

	if (_trueHdrFeature) {
		NVSDK_NGX_Parameter_SetUI(_ngxParameters, NVSDK_NGX_Parameter_TrueHDR_MaxLuminance,
			(uint32_t)std::lroundf(colorInfo.maxLuminance * SCENE_REFERRED_SDR_WHITE_LEVEL));
	}
}

}

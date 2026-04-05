#include "pch.h"
#include "CatmullRomDrawer.h"
#include "DescriptorHeap.h"
#include "EffectsDrawer.h"
#include "GraphicsContext.h"
#include "Logger.h"
#include "ScalingWindow.h"
#include "StrHelper.h"

namespace Magpie {

EffectsDrawer::~EffectsDrawer() noexcept {
#ifdef _DEBUG
	if (_rtxTrueHdrOutputDescriptorBaseOffset != std::numeric_limits<uint32_t>::max()) {
		_graphicsContext->GetDescriptorHeap()
			.Free(_rtxTrueHdrOutputDescriptorBaseOffset, 1);
	}
#endif
}

bool EffectsDrawer::Initialize(
	GraphicsContext& graphicsContext,
	const ColorInfo& colorInfo,
	Size inputSize,
	Size rendererSize
) noexcept {
	_graphicsContext = &graphicsContext;
	_colorInfo = colorInfo;
	_inputSize = inputSize;

	ID3D12Device5* device = graphicsContext.GetDevice();

	// 检查半精度浮点支持
	{
		D3D12_FEATURE_DATA_D3D12_OPTIONS featureData{};
		HRESULT hr = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &featureData, sizeof(featureData));
		if (SUCCEEDED(hr)) {
			_isFP16Supported = featureData.MinPrecisionSupport & D3D12_SHADER_MIN_PRECISION_SUPPORT_16_BIT;
			Logger::Get().Info(StrHelper::Concat("FP16 支持: ", _isFP16Supported ? "是" : "否"));
		} else {
			Logger::Get().ComError("CheckFeatureSupport 失败", hr);
		}
	}

	if (ScalingWindow::Get().Options().IsWindowedMode()) {
		_outputSize = rendererSize;
	} else {
		const float fillScale = std::min(
			float(rendererSize.width) / inputSize.width,
			float(rendererSize.height) / inputSize.height
		);
		_outputSize.width = std::lroundf(inputSize.width * fillScale);
		_outputSize.height = std::lroundf(inputSize.height * fillScale);
	}

#ifdef MP_ENABLE_RTX_TRUE_HDR
	if (colorInfo.kind == winrt::AdvancedColorKind::HighDynamicRange) {
		_rtxTrueHdrDrawer.emplace();
		HRESULT hr = _rtxTrueHdrDrawer->Initialize(graphicsContext, inputSize, colorInfo);
		if (FAILED(hr)) {
			Logger::Get().ComError("RtxTrueHdrDrawer::Initialize 失败", hr);
			return false;
		}

		Size rtxTrueHdrOutputSize = _rtxTrueHdrDrawer->GetOutputSize();

		if (rtxTrueHdrOutputSize != _outputSize) {
			{
				CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
				D3D12_HEAP_FLAGS heapFlag = graphicsContext.IsHeapFlagCreateNotZeroedSupported() ?
					D3D12_HEAP_FLAG_CREATE_NOT_ZEROED : D3D12_HEAP_FLAG_NONE;
				CD3DX12_RESOURCE_DESC texDesc = CD3DX12_RESOURCE_DESC::Tex2D(
					DXGI_FORMAT_R16G16B16A16_FLOAT,
					rtxTrueHdrOutputSize.width,
					rtxTrueHdrOutputSize.height,
					1, 1, 1, 0,
					D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
				);
				hr = device->CreateCommittedResource(
					&heapProps, heapFlag, &texDesc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
					nullptr, IID_PPV_ARGS(&_rtxTrueHdrOutput));
				if (FAILED(hr)) {
					return false;
				}
			}
			
			auto& descriptorHeap = graphicsContext.GetDescriptorHeap();

			hr = descriptorHeap.Alloc(1, _rtxTrueHdrOutputDescriptorBaseOffset);
			if (FAILED(hr)) {
				return false;
			}

			CD3DX12_SHADER_RESOURCE_VIEW_DESC srcDesc =
				CD3DX12_SHADER_RESOURCE_VIEW_DESC::Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT, 1);
			device->CreateShaderResourceView(_rtxTrueHdrOutput.get(), &srcDesc,
				descriptorHeap.GetCpuHandle(_rtxTrueHdrOutputDescriptorBaseOffset));
		}
	}
#endif

	_catmullRomDrawer.emplace();
	_catmullRomDrawer->Initialize(graphicsContext);

	{
		// 每帧两个时间戳
		const uint32_t timestampCount = 2 * ScalingWindow::Get().Options().maxProducerInFlightFrames;

		D3D12_QUERY_HEAP_DESC queryHeapDesc = {
			.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP,
			.Count = timestampCount
		};
		HRESULT hr = device->CreateQueryHeap(&queryHeapDesc, IID_PPV_ARGS(&_queryHeap));
		if (FAILED(hr)) {
			Logger::Get().ComError("CreateQueryHeap 失败", hr);
			return false;
		}

		CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_READBACK);
		CD3DX12_RESOURCE_DESC bufferDesc =
			CD3DX12_RESOURCE_DESC::Buffer(timestampCount * sizeof(UINT64));
		hr = device->CreateCommittedResource(
			&heapProps,
			D3D12_HEAP_FLAG_NONE,
			&bufferDesc,
			D3D12_RESOURCE_STATE_COPY_DEST,
			nullptr,
			IID_PPV_ARGS(&_queryResultBuffer)
		);
		if (FAILED(hr)) {
			Logger::Get().ComError("CreateCommittedResource 失败", hr);
			return false;
		}

		hr = graphicsContext.GetCommandQueue()->GetTimestampFrequency(&_timestampFrequency);
		if (FAILED(hr)) {
			Logger::Get().ComError("ID3D12CommandQueue::GetTimestampFrequency 失败", hr);
			return false;
		}
	}

	return true;
}

HRESULT EffectsDrawer::Draw(
	uint32_t frameIndex,
	ID3D12Resource* /*inputResource*/,
	ID3D12Resource* outputResource,
	uint32_t inputSrvOffset,
	uint32_t outputUavOffset
) noexcept {
	// 获取渲染时间
	const uint32_t queryHeapIndex = 2 * frameIndex;
	{
		CD3DX12_RANGE range(queryHeapIndex * sizeof(UINT64), (queryHeapIndex + 2) * sizeof(UINT64));

		void* pData;
		HRESULT hr = _queryResultBuffer->Map(0, nullptr, &pData);
		if (FAILED(hr)) {
			Logger::Get().ComError("ID3D12Resource::Map 失败", hr);
			return hr;
		}

		// UINT64* timestampes = (UINT64*)pData + queryHeapIndex;

		range = {};
		_queryResultBuffer->Unmap(0, &range);
	}

	ID3D12GraphicsCommandList* commandList = _graphicsContext->GetCommandList();

	commandList->EndQuery(_queryHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, queryHeapIndex);

	Size curInputSize = _inputSize;
	bool postResize = true;

#ifdef MP_ENABLE_RTX_TRUE_HDR
	if (_colorInfo.kind == winrt::AdvancedColorKind::HighDynamicRange) {
		if (!_rtxTrueHdrOutput) {
			postResize = false;
		}

		HRESULT hr = _rtxTrueHdrDrawer->Draw(
			inputSrvOffset, _rtxTrueHdrOutput ? _rtxTrueHdrOutput.get() : outputResource);
		if (FAILED(hr)) {
			return hr;
		}

		if (_rtxTrueHdrOutput) {
			inputSrvOffset = _rtxTrueHdrOutputDescriptorBaseOffset;
			curInputSize = _rtxTrueHdrDrawer->GetOutputSize();
		}
	}
#endif

	if (postResize) {
		_graphicsContext->SetDescriptorHeap(_graphicsContext->GetDescriptorHeap().GetHeap());

		_catmullRomDrawer->Draw(
			curInputSize,
			_outputSize,
			inputSrvOffset,
			outputUavOffset,
			false
		);
	}

	commandList->EndQuery(_queryHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, queryHeapIndex + 1);
	commandList->ResolveQueryData(_queryHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, queryHeapIndex, 2,
		_queryResultBuffer.get(), queryHeapIndex * sizeof(UINT64));

	return S_OK;
}

void EffectsDrawer::OnResized(Size rendererSize) noexcept {
	_outputSize = rendererSize;
}

void EffectsDrawer::OnColorInfoChanged(const ColorInfo& colorInfo) noexcept {
	_colorInfo = colorInfo;

#ifdef MP_ENABLE_RTX_TRUE_HDR
	if (_rtxTrueHdrDrawer) {
		_rtxTrueHdrDrawer->OnColorInfoChanged(colorInfo);
	}
#endif
}

}

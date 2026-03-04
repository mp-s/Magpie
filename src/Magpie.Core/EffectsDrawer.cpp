#include "pch.h"
#include "EffectsDrawer.h"
#include "CatmullRomDrawer.h"
#include "GraphicsContext.h"
#include "Logger.h"
#include "ScalingWindow.h"
#include "StrHelper.h"

namespace Magpie {

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

	if (colorInfo.kind == winrt::AdvancedColorKind::HighDynamicRange) {
		_rtxTrueHdrDrawer.emplace();
		HRESULT hr = _rtxTrueHdrDrawer->Initialize(graphicsContext, inputSize, colorInfo);
		if (FAILED(hr)) {
			Logger::Get().ComError("RtxTrueHdrDrawer::Initialize 失败", hr);
			return false;
		}
	}

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
	ID3D12Resource* /*outputResource*/,
	ID3D12DescriptorHeap* heap,
	D3D12_GPU_DESCRIPTOR_HANDLE inputSrvHandle,
	D3D12_GPU_DESCRIPTOR_HANDLE outputUavHandle
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

	if (_colorInfo.kind == winrt::AdvancedColorKind::HighDynamicRange) {
		HRESULT hr = _rtxTrueHdrDrawer->Draw();
		if (FAILED(hr)) {
			return hr;
		}
	}

	commandList->SetDescriptorHeaps(1, &heap);
	_catmullRomDrawer->Draw(_inputSize, _outputSize, inputSrvHandle, outputUavHandle, false);

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

	if (_rtxTrueHdrDrawer) {
		_rtxTrueHdrDrawer->OnColorInfoChanged(colorInfo);
	}
}

}

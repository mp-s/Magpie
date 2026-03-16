#include "pch.h"
#include "EffectsDrawer.h"
#include "CatmullRomDrawer.h"
#include "CommandContext.h"
#include "D3D12Context.h"
#include "Logger.h"
#include "ScalingWindow.h"
#include "StrHelper.h"

namespace Magpie {

bool EffectsDrawer::Initialize(
	D3D12Context& d3d12Context,
	const ColorInfo& colorInfo,
	Size inputSize,
	Size rendererSize
) noexcept {
	_d3d12Context = &d3d12Context;
	_isScRGB = colorInfo.kind != winrt::AdvancedColorKind::StandardDynamicRange;
	_inputSize = inputSize;

	ID3D12Device5* device = d3d12Context.GetDevice();

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

	_catmullRomDrawer.emplace();
	_catmullRomDrawer->Initialize(d3d12Context);

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

		hr = d3d12Context.GetCommandQueue()->GetTimestampFrequency(&_timestampFrequency);
		if (FAILED(hr)) {
			Logger::Get().ComError("ID3D12CommandQueue::GetTimestampFrequency 失败", hr);
			return false;
		}
	}

	return true;
}

HRESULT EffectsDrawer::Draw(
	ComputeContext& computeContext,
	uint32_t /*frameIndex*/,
	ID3D12Resource* /*inputResource*/,
	ID3D12Resource* /*outputResource*/,
	uint32_t inputSrvOffset,
	uint32_t outputUavOffset
) noexcept {
	// 获取渲染时间
	// const uint32_t queryHeapIndex = 2 * frameIndex;
	// {
	// 	CD3DX12_RANGE range(queryHeapIndex * sizeof(UINT64), (queryHeapIndex + 2) * sizeof(UINT64));
	   
	// 	void* pData;
	// 	HRESULT hr = _queryResultBuffer->Map(0, nullptr, &pData);
	// 	if (FAILED(hr)) {
	// 		Logger::Get().ComError("ID3D12Resource::Map 失败", hr);
	// 		return hr;
	// 	}
	   
	// 	UINT64* timestampes = (UINT64*)pData + queryHeapIndex;
	   
	// 	range = {};
	// 	_queryResultBuffer->Unmap(0, &range);
	// }

	//commandList->EndQuery(_queryHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, queryHeapIndex);

	_catmullRomDrawer->Draw(
		computeContext, _inputSize, _outputSize, inputSrvOffset, outputUavOffset, false);

	// commandList->EndQuery(_queryHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, queryHeapIndex + 1);
	// commandList->ResolveQueryData(_queryHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, queryHeapIndex, 2,
	//	_queryResultBuffer.get(), queryHeapIndex * sizeof(UINT64));

	return S_OK;
}

void EffectsDrawer::OnResized(Size rendererSize) noexcept {
	_outputSize = rendererSize;
}

void EffectsDrawer::OnColorInfoChanged(const ColorInfo& colorInfo) noexcept {
	_isScRGB = colorInfo.kind != winrt::AdvancedColorKind::StandardDynamicRange;
}

}

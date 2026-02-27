#include "pch.h"
#include "EffectsDrawer.h"
#include "CatmullRomDrawer.h"
#include "DynamicDescriptorHeap.h"
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
	_isScRGB = colorInfo.kind != winrt::AdvancedColorKind::StandardDynamicRange;
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

	_catmullRomDrawer.emplace();
	HRESULT hr = _catmullRomDrawer->Initialize(graphicsContext);
	if (FAILED(hr)) {
		Logger::Get().ComError("CatmullRomDrawer::Initialize 失败", hr);
		return false;
	}

	{
		// 每帧两个时间戳
		const uint32_t timestampCount = 2 * ScalingWindow::Get().Options().maxProducerInFlightFrames;

		D3D12_QUERY_HEAP_DESC queryHeapDesc = {
			.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP,
			.Count = timestampCount
		};
		hr = device->CreateQueryHeap(&queryHeapDesc, IID_PPV_ARGS(&_queryHeap));
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

HRESULT EffectsDrawer::Draw(uint32_t frameIndex, uint32_t inputSrvIdx, uint32_t outputUavIdx) noexcept {
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

	auto& dynamicDescriptorHeap = _graphicsContext->GetDynamicDescriptorHeap();
	const uint32_t descriptorSize = dynamicDescriptorHeap.GetDescriptorSize();
	D3D12_GPU_DESCRIPTOR_HANDLE heapGpuHandle;
	{
		ID3D12DescriptorHeap* heap = dynamicDescriptorHeap.GetHeapForBinding(heapGpuHandle);
		commandList->SetDescriptorHeaps(1, &heap);
	}

	commandList->EndQuery(_queryHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, queryHeapIndex);

	_catmullRomDrawer->Draw(
		_inputSize,
		_outputSize,
		CD3DX12_GPU_DESCRIPTOR_HANDLE(heapGpuHandle, inputSrvIdx, descriptorSize),
		CD3DX12_GPU_DESCRIPTOR_HANDLE(heapGpuHandle, outputUavIdx, descriptorSize),
		false
	);

	commandList->EndQuery(_queryHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, queryHeapIndex + 1);
	commandList->ResolveQueryData(_queryHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, queryHeapIndex, 2,
		_queryResultBuffer.get(), queryHeapIndex * sizeof(UINT64));

	return S_OK;
}

HRESULT EffectsDrawer::OnResized(Size rendererSize) noexcept {
	_outputSize = rendererSize;
	return S_OK;
}

HRESULT EffectsDrawer::OnColorInfoChanged(const ColorInfo& colorInfo) noexcept {
	_isScRGB = colorInfo.kind != winrt::AdvancedColorKind::StandardDynamicRange;
	return S_OK;
}

}

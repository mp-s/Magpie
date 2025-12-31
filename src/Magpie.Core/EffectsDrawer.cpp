#include "pch.h"
#include "EffectsDrawer.h"
#include "CatmullRomEffectDrawer.h"
#include "GraphicsContext.h"
#include "Logger.h"
#include "ScalingWindow.h"
#include "StrHelper.h"

namespace Magpie {

bool EffectsDrawer::Initialize(
	GraphicsContext& graphicsContext,
	const ColorInfo& colorInfo,
	Size inputSize,
	Size rendererSize,
	const SmallVectorImpl<ID3D12Resource*>& inputResources,
	SmallVectorImpl<winrt::com_ptr<ID3D12Resource>>& outputResources
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

	_descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	const uint32_t maxInFlightFrameCount = ScalingWindow::Get().Options().maxProducerInFlightFrames;
	assert((uint32_t)inputResources.size() == maxInFlightFrameCount);

	{
		// 输入 SRV + 输出 UAV
		uint32_t descriptorCount = maxInFlightFrameCount + (maxInFlightFrameCount + 1);
		descriptorCount += CatmullRomEffectDrawer::CalcDescriptorCount(true, true);

		D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {
			.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
			.NumDescriptors = descriptorCount,
			.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
		};
		HRESULT hr = device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&_descriptorHeap));
		if (FAILED(hr)) {
			Logger::Get().ComError("CreateDescriptorHeap 失败", hr);
			return false;
		}
	}

	CD3DX12_CPU_DESCRIPTOR_HANDLE descriptorCpuHandle(
		_descriptorHeap->GetCPUDescriptorHandleForHeapStart());
	CD3DX12_GPU_DESCRIPTOR_HANDLE descriptorGpuHandle(
		_descriptorHeap->GetGPUDescriptorHandleForHeapStart());
	_inputDescriptorCpuBase = _descriptorHeap->GetCPUDescriptorHandleForHeapStart();
	_inputDescriptorGpuBase = _descriptorHeap->GetGPUDescriptorHandleForHeapStart();

	HRESULT hr = _CreateInputResources(inputResources);
	if (FAILED(hr)) {
		Logger::Get().ComError("_CreateInputResources 失败", hr);
		return false;
	}

	descriptorCpuHandle.Offset(maxInFlightFrameCount, _descriptorSize);
	descriptorGpuHandle.Offset(maxInFlightFrameCount, _descriptorSize);
	_outputDescriptorCpuBase = descriptorCpuHandle;
	_outputDescriptorGpuBase = descriptorGpuHandle;

	descriptorCpuHandle.Offset(maxInFlightFrameCount + 1, _descriptorSize);
	descriptorGpuHandle.Offset(maxInFlightFrameCount + 1, _descriptorSize);
	_effectsDescriptorCpuBase = descriptorCpuHandle;
	_effectsDescriptorGpuBase = descriptorGpuHandle;

	_outputSize = rendererSize;

	{
		EffectColorSpace colorSpace = colorInfo.kind == winrt::AdvancedColorKind::StandardDynamicRange ?
			EffectColorSpace::sRGB : EffectColorSpace::scRGB;
		
		hr = _catmullRom.Initialize(graphicsContext, _inputSize, _outputSize,
			colorSpace, colorSpace, descriptorCpuHandle, descriptorGpuHandle,
			_descriptorSize, nullptr, nullptr);
		if (FAILED(hr)) {
			Logger::Get().ComError("CatmullRomEffectDrawer::Initialize 失败", hr);
			return false;
		}
	}
	
	hr = _CreateOutputResources(outputResources);
	if (FAILED(hr)) {
		Logger::Get().ComError("_CreateOutputResources 失败", hr);
		return false;
	}

	{
		// 每帧两个时间戳
		const uint32_t timestampCount = 2 * maxInFlightFrameCount;

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

HRESULT EffectsDrawer::Draw(uint32_t frameIndex, uint32_t inputIndx, uint32_t outputIndex) noexcept {
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

	{
		ID3D12DescriptorHeap* t = _descriptorHeap.get();
		commandList->SetDescriptorHeaps(1, &t);
	}

	commandList->EndQuery(_queryHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, queryHeapIndex);

	_catmullRom.Draw(
		CD3DX12_GPU_DESCRIPTOR_HANDLE(_inputDescriptorGpuBase, inputIndx, _descriptorSize),
		CD3DX12_GPU_DESCRIPTOR_HANDLE(_outputDescriptorGpuBase, outputIndex, _descriptorSize)
	);

	commandList->EndQuery(_queryHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, queryHeapIndex + 1);
	commandList->ResolveQueryData(_queryHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, queryHeapIndex, 2,
		_queryResultBuffer.get(), queryHeapIndex * sizeof(UINT64));

	return S_OK;
}

HRESULT EffectsDrawer::OnResized(
	Size rendererSize,
	SmallVectorImpl<winrt::com_ptr<ID3D12Resource>>& outputResources
) noexcept {
	_outputSize = rendererSize;

	HRESULT hr = _CreateOutputResources(outputResources);
	if (FAILED(hr)) {
		Logger::Get().ComError("_CreateOutputResources 失败", hr);
		return hr;
	}

	CD3DX12_CPU_DESCRIPTOR_HANDLE descriptorCpuHandle = _effectsDescriptorCpuBase;
	CD3DX12_GPU_DESCRIPTOR_HANDLE descriptorGpuHandle = _effectsDescriptorGpuBase;
	_catmullRom.OnResized(_inputSize, _outputSize, descriptorCpuHandle,
		descriptorGpuHandle, _descriptorSize, nullptr, nullptr);
	
	return S_OK;
}

HRESULT EffectsDrawer::OnColorInfoChanged(
	const ColorInfo& colorInfo,
	const SmallVectorImpl<ID3D12Resource*>& inputResources,
	SmallVectorImpl<winrt::com_ptr<ID3D12Resource>>& outputResources
) noexcept {
	const bool wasScRGB = _isScRGB;
	_isScRGB = colorInfo.kind != winrt::AdvancedColorKind::StandardDynamicRange;

	if (_isScRGB != wasScRGB) {
		HRESULT hr = _CreateInputResources(inputResources);
		if (FAILED(hr)) {
			Logger::Get().ComError("_CreateInputResources 失败", hr);
			return hr;
		}

		hr = _CreateOutputResources(outputResources);
		if (FAILED(hr)) {
			Logger::Get().ComError("_CreateOutputResources 失败", hr);
			return hr;
		}
	}

	EffectColorSpace colorSpace = colorInfo.kind == winrt::AdvancedColorKind::StandardDynamicRange ?
		EffectColorSpace::sRGB : EffectColorSpace::scRGB;
	CD3DX12_CPU_DESCRIPTOR_HANDLE descriptorCpuHandle = _effectsDescriptorCpuBase;
	CD3DX12_GPU_DESCRIPTOR_HANDLE descriptorGpuHandle = _effectsDescriptorGpuBase;
	_catmullRom.OnColorInfoChanged(colorSpace, colorSpace, descriptorCpuHandle,
		descriptorGpuHandle, _descriptorSize, nullptr, nullptr);
	
	return S_OK;
}

HRESULT EffectsDrawer::_CreateInputResources(
	const SmallVectorImpl<ID3D12Resource*>& inputResources
) const noexcept {
	ID3D12Device5* device = _graphicsContext->GetDevice();

	const uint32_t maxInFlightFrameCount = ScalingWindow::Get().Options().maxProducerInFlightFrames;
	assert((uint32_t)inputResources.size() == maxInFlightFrameCount);

	CD3DX12_SHADER_RESOURCE_VIEW_DESC srvDesc = CD3DX12_SHADER_RESOURCE_VIEW_DESC::Tex2D(
		_isScRGB ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, 1);

	CD3DX12_CPU_DESCRIPTOR_HANDLE descriptorCpuHandle = _inputDescriptorCpuBase;

	for (uint32_t i = 0; i < maxInFlightFrameCount; ++i) {
		device->CreateShaderResourceView(inputResources[i], &srvDesc, descriptorCpuHandle);
		descriptorCpuHandle.Offset(_descriptorSize);
	}

	return S_OK;
}

HRESULT EffectsDrawer::_CreateOutputResources(
	SmallVectorImpl<winrt::com_ptr<ID3D12Resource>>& outputResources
) const noexcept {
	ID3D12Device5* device = _graphicsContext->GetDevice();

	const uint32_t maxInFlightFrameCount =
		ScalingWindow::Get().Options().maxProducerInFlightFrames;
	outputResources.resize(size_t(maxInFlightFrameCount + 1));

	CD3DX12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE_DEFAULT);

	D3D12_HEAP_FLAGS heapFlag = _graphicsContext->IsHeapFlagCreateNotZeroedSupported() ?
		D3D12_HEAP_FLAG_CREATE_NOT_ZEROED : D3D12_HEAP_FLAG_NONE;

	CD3DX12_RESOURCE_DESC texDesc = CD3DX12_RESOURCE_DESC::Tex2D(
		_isScRGB ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM,
		_outputSize.width,
		_outputSize.height,
		1, 1, 1, 0,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
	);

	CD3DX12_UNORDERED_ACCESS_VIEW_DESC uavDesc = CD3DX12_UNORDERED_ACCESS_VIEW_DESC::Tex2D(
		_isScRGB ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM);

	CD3DX12_CPU_DESCRIPTOR_HANDLE outputDescriptorCpuHandle = _outputDescriptorCpuBase;

	for (winrt::com_ptr<ID3D12Resource>& resource : outputResources) {
		HRESULT hr = device->CreateCommittedResource(&heapProperties, heapFlag,
			&texDesc, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, IID_PPV_ARGS(&resource));
		if (FAILED(hr)) {
			Logger::Get().ComError("CreateCommittedResource 失败", hr);
			return hr;
		}

		device->CreateUnorderedAccessView(
			resource.get(), nullptr, &uavDesc, outputDescriptorCpuHandle);
		outputDescriptorCpuHandle.Offset(_descriptorSize);
	}

	return S_OK;
}

}

#include "pch.h"
#include "DuplicateFrameChecker.h"
#include "DirectXHelper.h"
#include "Logger.h"
#include "ScalingWindow.h"
#include "shaders/DuplicateFrameCS.h"

namespace Magpie {

static constexpr uint16_t INITIAL_CHECK_COUNT = 16;
static constexpr uint16_t INITIAL_SKIP_COUNT = 1;
static constexpr uint16_t MAX_SKIP_COUNT = 16;

DuplicateFrameChecker::DuplicateFrameChecker() noexcept :
	_nextSkipCount(INITIAL_SKIP_COUNT), _framesLeft(INITIAL_CHECK_COUNT) {}

bool DuplicateFrameChecker::Initialize(ID3D12Device5* device, const ColorInfo& colorInfo, Size frameSize, uint32_t frameCount) noexcept {
	assert(ScalingWindow::Get().Options().duplicateFrameDetectionMode !=
		DuplicateFrameDetectionMode::Never);

	_device = device;
	_isScRGB = colorInfo.kind != winrt::AdvancedColorKind::StandardDynamicRange;
	_frameSize = frameSize;

	{
		// 需要快速获取结果，因此使用高优先级
		D3D12_COMMAND_QUEUE_DESC queueDesc = {
			.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE,
			.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH
		};
		HRESULT hr = device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&_commandQueue));
		if (FAILED(hr)) {
			Logger::Get().ComError("CreateCommandQueue 失败", hr);
			return false;
		}
	}

	HRESULT hr = device->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_COMPUTE,
		D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(&_commandList));
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateCommandList1 失败", hr);
		return false;
	}

	hr = device->CreateCommandAllocator(
		D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&_commandAllocator));
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateCommandAllocator 失败", hr);
		return false;
	}

	{
		CD3DX12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE_DEFAULT);
		CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(
			MAX_CAPTURE_DIRTY_RECT_COUNT * sizeof(uint32_t), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

		hr = device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE,
			&desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&_resultBuffer));
		if (FAILED(hr)) {
			Logger::Get().ComError("CreateCommittedResource 失败", hr);
			return false;
		}

		heapProperties.Type = D3D12_HEAP_TYPE_READBACK;
		desc.Flags = D3D12_RESOURCE_FLAG_NONE;
		hr = device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE,
			&desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&_resultReadbackBuffer));
		if (FAILED(hr)) {
			Logger::Get().ComError("CreateCommittedResource 失败", hr);
			return false;
		}
	}

	{
		D3D12_DESCRIPTOR_HEAP_DESC desc = {
			.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
			.NumDescriptors = frameCount,
			.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
		};
		hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&_descriptorHeap));
		if (FAILED(hr)) {
			Logger::Get().ComError("CreateDescriptorHeap 失败", hr);
			return false;
		}
	}

	_descriptorTracker.resize(frameCount);

	_descriptorSize = device->GetDescriptorHandleIncrementSize(
		D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_fence));
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateFence 失败", hr);
		return false;
	}

	{
		// 检查根签名版本
		D3D12_FEATURE_DATA_ROOT_SIGNATURE versionData = {
			.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1
		};
		hr = device->CheckFeatureSupport(
			D3D12_FEATURE_ROOT_SIGNATURE, &versionData, sizeof(versionData));
		if (FAILED(hr)) {
			Logger::Get().ComError("CheckFeatureSupport 失败", hr);
			return false;
		}

		winrt::com_ptr<ID3DBlob> signature;

		CD3DX12_DESCRIPTOR_RANGE1 srvRange1 = CD3DX12_DESCRIPTOR_RANGE1(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0,
			D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE);
		CD3DX12_DESCRIPTOR_RANGE1 srvRange2 = CD3DX12_DESCRIPTOR_RANGE1(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0,
			D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE);

		D3D12_ROOT_PARAMETER1 rootParams[] = {
			{
				.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
				.Constants = {
					.Num32BitValues = 8
				}
			},
			{
				.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV,
				.Descriptor = {
					.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE
				}
			},
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
			}
		};

		D3D12_STATIC_SAMPLER_DESC samplerDesc = {
			.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT,
			.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
			.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
			.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
			.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER,
			.ShaderRegister = 0
		};

		CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc(
			(UINT)std::size(rootParams), rootParams, 1, &samplerDesc);
		hr = D3DX12SerializeVersionedRootSignature(
			&rootSignatureDesc, versionData.HighestVersion, signature.put(), nullptr);
		if (FAILED(hr)) {
			Logger::Get().ComError("D3DX12SerializeVersionedRootSignature 失败", hr);
			return false;
		}

		hr = device->CreateRootSignature(0, signature->GetBufferPointer(),
			signature->GetBufferSize(), IID_PPV_ARGS(&_rootSignature));
		if (FAILED(hr)) {
			Logger::Get().ComError("CreateRootSignature 失败", hr);
			return false;
		}
	}

	D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {
		.pRootSignature = _rootSignature.get(),
		.CS = CD3DX12_SHADER_BYTECODE(DuplicateFrameCS, sizeof(DuplicateFrameCS))
	};
	hr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&_pipelineState));
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateComputePipelineState 失败", hr);
		return false;
	}
	
	return true;
}

HRESULT DuplicateFrameChecker::CheckFrame(
	ID3D12Resource* frameResource,
	uint32_t frameIdx,
	SmallVectorImpl<Rect>& dirtyRects
) noexcept {
	assert(!dirtyRects.empty() && dirtyRects.size() <= MAX_CAPTURE_DIRTY_RECT_COUNT);

#ifdef _DEBUG
	{
		D3D12_RESOURCE_DESC desc = frameResource->GetDesc();
		assert(desc.Width == _frameSize.width && desc.Height == _frameSize.height);
	}
#endif

	if (!_descriptorTracker[frameIdx]) {
		CD3DX12_SHADER_RESOURCE_VIEW_DESC desc = CD3DX12_SHADER_RESOURCE_VIEW_DESC::Tex2D(
			_isScRGB ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM, 1);
		_device->CreateShaderResourceView(frameResource, &desc, CD3DX12_CPU_DESCRIPTOR_HANDLE(
			_descriptorHeap->GetCPUDescriptorHandleForHeapStart(), frameIdx, _descriptorSize));

		_descriptorTracker[frameIdx] = true;
	}

	// 第一帧无需检查重复帧
	if (_oldFrameIdx == std::numeric_limits<uint32_t>::max()) {
		return S_OK;
	}

	if (ScalingWindow::Get().Options().duplicateFrameDetectionMode == DuplicateFrameDetectionMode::Always) {
		HRESULT hr = _CheckDirtyRects(frameIdx, dirtyRects);
		if (FAILED(hr)) {
			Logger::Get().ComError("_CheckDirtyRects 失败", hr);
			return hr;
		}

		return S_OK;
	}

	// 动态检查重复帧，见 #787
	if (_isCheckingForDuplicateFrame) {
		if (--_framesLeft == 0) {
			_isCheckingForDuplicateFrame = false;
			_framesLeft = _nextSkipCount;
			if (_nextSkipCount < MAX_SKIP_COUNT) {
				// 增加下一次连续跳过检查的帧数
				++_nextSkipCount;
			}
		}

		HRESULT hr = _CheckDirtyRects(frameIdx, dirtyRects);
		if (FAILED(hr)) {
			Logger::Get().ComError("_CheckDirtyRects 失败", hr);
			return hr;
		}

		if (dirtyRects.empty()) {
			_isCheckingForDuplicateFrame = true;
			_framesLeft = INITIAL_CHECK_COUNT;
			_nextSkipCount = INITIAL_SKIP_COUNT;
		}
	} else {
		if (--_framesLeft == 0) {
			_isCheckingForDuplicateFrame = true;
			// 第 2 次连续检查 10 帧，之后逐渐减少，从第 16 次开始只连续检查 2 帧
			_framesLeft = uint32_t((-4 * (int)_nextSkipCount + 78) / 7);
		}
	}

	return S_OK;
}

void DuplicateFrameChecker::OnFrameAdopted(uint32_t frameIdx) noexcept {
	_oldFrameIdx = frameIdx;
}

void DuplicateFrameChecker::OnCaptureRestarted() noexcept {
	_oldFrameIdx = std::numeric_limits<uint32_t>::max();
	// 使描述符失效
	std::fill(_descriptorTracker.begin(), _descriptorTracker.end(), false);
}

HRESULT DuplicateFrameChecker::_CheckDirtyRects(uint32_t newFrameIdx, SmallVectorImpl<Rect>& dirtyRects) noexcept {
	HRESULT hr = _commandAllocator->Reset();
	if (FAILED(hr)) {
		Logger::Get().ComError("ID3D12CommandAllocator::Reset 失败", hr);
		return hr;
	}

	hr = _commandList->Reset(_commandAllocator.get(), _pipelineState.get());
	if (FAILED(hr)) {
		Logger::Get().ComError("ID3D12GraphicsCommandList::Reset 失败", hr);
		return hr;
	}

	{
		ID3D12DescriptorHeap* t = _descriptorHeap.get();
		_commandList->SetDescriptorHeaps(1, &t);
	}

	_commandList->SetComputeRootSignature(_rootSignature.get());

	_commandList->SetComputeRootUnorderedAccessView(1, _resultBuffer->GetGPUVirtualAddress());

	{
		D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = _descriptorHeap->GetGPUDescriptorHandleForHeapStart();
		_commandList->SetComputeRootDescriptorTable(
			2, CD3DX12_GPU_DESCRIPTOR_HANDLE(gpuHandle, _oldFrameIdx, _descriptorSize));
		_commandList->SetComputeRootDescriptorTable(
			3, CD3DX12_GPU_DESCRIPTOR_HANDLE(gpuHandle, newFrameIdx, _descriptorSize));
	}
	
	const uint32_t dirtyRectCount = (uint32_t)dirtyRects.size();
	for (uint32_t i = 0; i < dirtyRectCount; ++i) {
		const Rect& dirtyRect = dirtyRects[i];

		if (i == 0) {
			DirectXHelper::Constant32 constants[] = {
				{.floatVal = 1.0f / _frameSize.width},
				{.floatVal = 1.0f / _frameSize.height},
				{.uintVal = ++_curTargetValue},
				{.uintVal = i},
				{.uintVal = dirtyRect.left},
				{.uintVal = dirtyRect.top},
				{.uintVal = dirtyRect.right},
				{.uintVal = dirtyRect.bottom}
			};
			_commandList->SetComputeRoot32BitConstants(0, (UINT)std::size(constants), constants, 0);
		} else {
			DirectXHelper::Constant32 constants[] = {
				{.uintVal = i},
				{.uintVal = dirtyRect.left},
				{.uintVal = dirtyRect.top},
				{.uintVal = dirtyRect.right},
				{.uintVal = dirtyRect.bottom}
			};
			_commandList->SetComputeRoot32BitConstants(0, (UINT)std::size(constants), constants, 3);
		}

		_commandList->Dispatch(
			(dirtyRect.right - dirtyRect.left + DUP_FRAME_DISPATCH_BLOCK_SIZE - 1) / DUP_FRAME_DISPATCH_BLOCK_SIZE,
			(dirtyRect.bottom - dirtyRect.top + DUP_FRAME_DISPATCH_BLOCK_SIZE - 1) / DUP_FRAME_DISPATCH_BLOCK_SIZE,
			1
		);
	}

	{
		CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
			_resultBuffer.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE, 0);
		_commandList->ResourceBarrier(1, &barrier);
	}
	
	_commandList->CopyBufferRegion(_resultReadbackBuffer.get(), 0,
		_resultBuffer.get(), 0, dirtyRectCount * sizeof(uint32_t));

	hr = _commandList->Close();
	if (FAILED(hr)) {
		Logger::Get().ComError("ID3D12GraphicsCommandList::Close 失败", hr);
		return hr;
	}

	{
		ID3D12CommandList* t = _commandList.get();
		_commandQueue->ExecuteCommandLists(1, &t);
	}

	hr = _commandQueue->Signal(_fence.get(), ++_curFenceValue);
	if (FAILED(hr)) {
		Logger::Get().ComError("ID3D12CommandQueue::Signal 失败", hr);
		return hr;
	}

	hr = _fence->SetEventOnCompletion(_curFenceValue, NULL);
	if (FAILED(hr)) {
		Logger::Get().ComError("ID3D12Fence::SetEventOnCompletion 失败", hr);
		return hr;
	}

	// 读取结果
	SmallVector<uint32_t, 4> removeList;
	{
		CD3DX12_RANGE range(0, dirtyRectCount * sizeof(uint32_t));

		void* pData;
		hr = _resultReadbackBuffer->Map(0, nullptr, &pData);
		if (FAILED(hr)) {
			Logger::Get().ComError("ID3D12Resource::Map 失败", hr);
			return hr;
		}

		for (uint32_t i = 0; i < dirtyRectCount; ++i) {
			if (((uint32_t*)pData)[i] != _curTargetValue) {
				// 此矩形内画面无变化
				removeList.push_back(i);
			}
		}
		
		range = {};
		_resultReadbackBuffer->Unmap(0, &range);
	}

	if (!removeList.empty()) {
		// 从后向前删除
		std::sort(removeList.begin(), removeList.end(), std::greater<uint32_t>());
		for (uint32_t idx : removeList) {
			dirtyRects.erase(dirtyRects.begin() + idx);
		}
	}

	return S_OK;
}

}

#pragma once
#include "ScalingOptions.h"

namespace Magpie {

class GraphicsContext {
public:
	GraphicsContext() = default;
	GraphicsContext(const GraphicsContext&) = delete;
	GraphicsContext(GraphicsContext&&) = delete;

	bool Initialize(
		const GraphicsCardId& graphicsCardId,
		uint32_t maxInFlightFrameCount,
		D3D12_COMMAND_LIST_TYPE commandListType
	) noexcept;

	bool Initialize(
		ID3D12Device5* device,
		uint32_t maxInFlightFrameCount,
		D3D12_COMMAND_LIST_TYPE commandListType
	) noexcept;

	IDXGIFactory7* GetDXGIFactory() const noexcept {
		return _dxgiFactory.get();
	}

	IDXGIFactory7* GetDXGIFactoryForEnumingAdapters() noexcept;

	IDXGIAdapter4* GetDXGIAdapter() const noexcept {
		return _dxgiAdapter.get();
	}

	ID3D12Device5* GetDevice() const noexcept {
		return _device.get();
	}

	ID3D12CommandQueue* GetCommandQueue() const noexcept {
		return _commandQueue.get();
	}

	ID3D12GraphicsCommandList* GetCommandList() const noexcept {
		return _commandList.get();
	}

	D3D_ROOT_SIGNATURE_VERSION GetRootSignatureVersion() const noexcept {
		return _rootSignatureVersion;
	}

	uint32_t GetMaxInFlightFrameCount() const noexcept {
		return (uint32_t)_commandAllocators.size();
	}

	HRESULT Signal(uint64_t& fenceValue) noexcept;

	HRESULT WaitForFenceValue(uint64_t fenceValue) noexcept;

	HRESULT WaitForGPU() noexcept;

	HRESULT BeginFrame(
		uint32_t& curFrameIndex,
		ID3D12PipelineState* initialState = nullptr
	) noexcept;

	HRESULT EndFrame() noexcept;

private:
	HRESULT _CreateDXGIFactory() noexcept;

	bool _InitializeDeviceResources(uint32_t maxInFlightFrameCount, D3D12_COMMAND_LIST_TYPE commandListType) noexcept;

	bool _CreateAdapterAndDevice(const GraphicsCardId& graphicsCardId) noexcept;

	bool _TryCreateD3DDevice(const winrt::com_ptr<IDXGIAdapter1>& adapter, const DXGI_ADAPTER_DESC1& adapterDesc) noexcept;

	bool _CreateAdapterFromDevice() noexcept;

	winrt::com_ptr<IDXGIFactory7> _dxgiFactory;
	winrt::com_ptr<IDXGIAdapter4> _dxgiAdapter;
	winrt::com_ptr<ID3D12Device5> _device;
	winrt::com_ptr<ID3D12CommandQueue> _commandQueue;

	std::vector<winrt::com_ptr<ID3D12CommandAllocator>> _commandAllocators;
	winrt::com_ptr<ID3D12GraphicsCommandList> _commandList;

	winrt::com_ptr<ID3D12Fence1> _fence;
	uint64_t _curFenceValue = 0;

	std::vector<uint64_t> _frameFenceValues;
	uint32_t _curFrameIndex = 0;

	D3D_ROOT_SIGNATURE_VERSION _rootSignatureVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
};

}

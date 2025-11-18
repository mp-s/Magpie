#pragma once

namespace Magpie {

enum class FrameSourceState {
	NewFrame,
	Waiting,
	Error
};

class GraphicsCaptureFrameSource2 {
public:
	GraphicsCaptureFrameSource2() = default;

	// 不可复制，不可移动
	GraphicsCaptureFrameSource2(const GraphicsCaptureFrameSource2&) = delete;
	GraphicsCaptureFrameSource2(GraphicsCaptureFrameSource2&&) = delete;

	bool Initialize(
		ID3D12Device5* device,
		ID3D12CommandList* commandList,
		HMONITOR hMonSrc,
		bool useScRGB
	) noexcept;

	FrameSourceState Update() noexcept;
};

}

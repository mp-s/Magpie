#pragma once

namespace Magpie {

class GraphicsCaptureFrameSource2 {
public:
	// 不可复制，不可移动
	GraphicsCaptureFrameSource2(const GraphicsCaptureFrameSource2&) = delete;
	GraphicsCaptureFrameSource2(GraphicsCaptureFrameSource2&&) = delete;

	bool Initialize(
		ID3D12Device5* device,
		ID3D12CommandQueue* commandQueue,
		uint32_t bufferCount,
		HMONITOR hMonSrc,
		bool useScRGB
	) noexcept;
};

}

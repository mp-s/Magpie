#include "pch.h"
#include "GraphicsCaptureFrameSource2.h"

namespace Magpie {

bool GraphicsCaptureFrameSource2::Initialize(
	ID3D12Device5* /*device*/,
	ID3D12CommandQueue* /*commandQueue*/,
	uint32_t /*bufferCount*/,
	HMONITOR /*hMonSrc*/,
	bool /*useScRGB*/
) noexcept {
	return false;
}

}

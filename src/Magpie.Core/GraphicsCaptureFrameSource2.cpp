#include "pch.h"
#include "GraphicsCaptureFrameSource2.h"

namespace Magpie {

bool GraphicsCaptureFrameSource2::Initialize(
	ID3D12Device5* /*device*/,
	ID3D12CommandList* /*commandList*/,
	HMONITOR /*hMonSrc*/,
	bool /*useScRGB*/
) noexcept {
	return true;
}

FrameSourceState GraphicsCaptureFrameSource2::Update() noexcept {
	return FrameSourceState::NewFrame;
}

}

#include "pch.h"
#include "Renderer2.h"

namespace Magpie {

Renderer2::Renderer2() noexcept {}

Renderer2::~Renderer2() noexcept {}

ScalingError Renderer2::Initialize(HWND /*hwndAttach*/, OverlayOptions& /*overlayOptions*/) noexcept {
	return ScalingError();
}

}

#include "pch.h"
#include "CursorDrawer2.h"

namespace Magpie {

bool CursorDrawer2::Initialize(GraphicsContext& graphicsContext) noexcept {
	_graphicsContext = &graphicsContext;

	return true;
}

}

#pragma once

namespace Magpie {

class GraphicsContext;

class CursorDrawer2 {
public:
	CursorDrawer2() noexcept = default;
	CursorDrawer2(const CursorDrawer2&) = delete;
	CursorDrawer2(CursorDrawer2&&) = delete;

	bool Initialize(GraphicsContext& graphicsContext) noexcept;

private:
	GraphicsContext* _graphicsContext = nullptr;
};

}

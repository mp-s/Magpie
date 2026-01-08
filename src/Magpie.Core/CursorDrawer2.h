#pragma once

namespace Magpie {

class GraphicsContext;

class CursorDrawer2 {
public:
	CursorDrawer2() noexcept = default;
	CursorDrawer2(const CursorDrawer2&) = delete;
	CursorDrawer2(CursorDrawer2&&) = delete;

	bool Initialize(GraphicsContext& graphicsContext) noexcept;

	bool NeedRedraw(HCURSOR& hCursor, POINT cursorPos) const noexcept;

	void Draw(HCURSOR hCursor, POINT cursorPos) noexcept;

	void OnCursorVirtualizationStarted() noexcept {
		_isCursorVirtualized = true;
	}

	void OnCursorVirtualizationEnded() noexcept {
		_isCursorVirtualized = false;
	}

	void OnMoveStarted() noexcept {
		_isMoving = true;
	}

	void OnMoveEnded() noexcept {
		_isMoving = false;
	}

	void OnSrcMoveStarted() noexcept {
		_isSrcMoving = true;
	}

	void OnSrcMoveEnded() noexcept {
		_isSrcMoving = false;
	}

private:
	void _GetCursorState(HCURSOR& hCursor, POINT cursorPos, bool& isActive) const noexcept;

	GraphicsContext* _graphicsContext = nullptr;

	// 这两个成员用于检查自动隐藏光标
	HCURSOR _lastRawCursorHandle = NULL;
	std::chrono::steady_clock::time_point _lastCursorActiveTime;
	// 上次绘制的光标形状和位置
	HCURSOR _lastCursorHandle = NULL;
	POINT _lastCursorPos{ std::numeric_limits<LONG>::max(), std::numeric_limits<LONG>::max() };

	bool _isCursorVisible = true;
	bool _isMoving = false;
	bool _isCursorVirtualized = false;
	bool _isSrcMoving = false;
};

}

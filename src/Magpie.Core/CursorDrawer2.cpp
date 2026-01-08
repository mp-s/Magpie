#include "pch.h"
#include "CursorDrawer2.h"
#include "ScalingWindow.h"
#include "Win32Helper.h"

namespace Magpie {

bool CursorDrawer2::Initialize(GraphicsContext& graphicsContext) noexcept {
	_graphicsContext = &graphicsContext;

	return true;
}

bool CursorDrawer2::NeedRedraw(HCURSOR& hCursor, POINT cursorPos) const noexcept {
	bool isCursorActive = false;
	_GetCursorState(hCursor, cursorPos, isCursorActive);

	// 检查光标是否在视口内
	
	// 光标形状或位置变化时需要重新绘制
	return hCursor != _lastCursorHandle || (hCursor && cursorPos != _lastCursorPos);
}

void CursorDrawer2::Draw(HCURSOR hCursor, POINT cursorPos) noexcept {

}

void CursorDrawer2::_GetCursorState(HCURSOR& hCursor, POINT cursorPos, bool& isActive) const noexcept {
	assert(!isActive);
	using namespace std::chrono;

	const ScalingWindow& scalingWindow = ScalingWindow::Get();
	const ScalingOptions& options = scalingWindow.Options();

	// 检查自动隐藏光标
	if (options.autoHideCursorDelay.has_value()) {
		// 光标在叠加层上或拖动窗口时禁用自动隐藏。光标处于隐藏状态视为形状不变，考虑形状
		// 变化：箭头->隐藏->箭头，只要位置不变，自动隐藏功能应让光标始终隐藏；反之如果光
		// 标隐藏时移动了或显示时形状变化了应正常显示。
		if (_isCursorVirtualized &&
			!_isMoving &&
			!_isSrcMoving &&
			_lastCursorPos == cursorPos &&
			(_lastRawCursorHandle == hCursor || !hCursor)
		) {
			const duration<float> hideDelay(*options.autoHideCursorDelay);
			if (steady_clock::now() - _lastCursorActiveTime > hideDelay) {
				hCursor = NULL;
			}
		} else {
			isActive = true;
		}
	}

	// 截屏时暂时不渲染光标
	if (!_isCursorVisible) {
		hCursor = NULL;
	}
}

}

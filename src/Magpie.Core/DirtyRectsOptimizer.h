#pragma once
#include <SmallVector.h>

namespace Magpie {

struct DirtyRectsOptimizer {
	// 尝试减少脏矩形数量和总像素数
	static void Execute(SmallVectorImpl<Rect>& dirtyRects, uint32_t dirtyRectCountLimit) noexcept;
};

}

#include "pch.h"
#include "DirtyRectsOptimizer.h"
#include "RectHelper.h"

namespace Magpie {

static bool IsCornerInRect(Point p, const Rect& r) noexcept {
	return p.x >= r.left && p.x <= r.right && p.y >= r.top && p.y <= r.bottom;
}

static bool OptimizeDirtyRectPair(Rect& rect1, Rect& rect2, bool reversed = false) noexcept {
	if (RectHelper::IsEmpty(rect1) || RectHelper::IsEmpty(rect2)) {
		return false;
	}

	// 计算 rect2 有几个角在 rect1 内
	bool lt = IsCornerInRect(Point{ rect2.left, rect2.top }, rect1);
	bool rt = IsCornerInRect(Point{ rect2.right, rect2.top }, rect1);
	bool rb = IsCornerInRect(Point{ rect2.right, rect2.bottom }, rect1);
	bool lb = IsCornerInRect(Point{ rect2.left, rect2.bottom }, rect1);
	uint32_t count = (uint32_t)lt + (uint32_t)rt + (uint32_t)rb + (uint32_t)lb;

	if (count == 0) {
		// 尝试反向
		if (!reversed) {
			return OptimizeDirtyRectPair(rect2, rect1, true);
		}
	} else if (count == 2) {
		// rect2 有两个角在 rect1 内时可以合并或裁剪
		if (lt) {
			if (rt) {
				if (rect2.left == rect1.left && rect2.right == rect1.right) {
					// rect2 合并进 rect1
					rect1.bottom = rect2.bottom;
					rect2.right = rect2.left;
					return true;
				} else if (rect2.top != rect1.bottom) {
					// 裁剪 rect2
					rect2.top = rect1.bottom;
					assert(rect2.bottom >= rect2.top);
					return true;
				}
			} else {
				assert(lb);
				if (rect2.top == rect1.top && rect2.bottom == rect1.bottom) {
					rect1.right = rect2.right;
					rect2.right = rect2.left;
					return true;
				} else if (rect2.left != rect1.right) {
					rect2.left = rect1.right;
					assert(rect2.right >= rect2.left);
					return true;
				}
			}
		} else {
			assert(rb);
			if (rt) {
				if (rect2.top == rect1.top && rect2.bottom == rect1.bottom) {
					rect1.left = rect2.left;
					rect2.right = rect2.left;
					return true;
				} else if (rect2.right != rect1.left) {
					rect2.right = rect1.left;
					assert(rect2.right >= rect2.left);
					return true;
				}
			} else {
				if (rect2.left == rect1.left && rect2.right == rect1.right) {
					rect1.top = rect2.top;
					rect2.right = rect2.left;
					return true;
				} else if (rect2.bottom != rect1.top) {
					rect2.bottom = rect1.top;
					assert(rect2.bottom >= rect2.top);
					return true;
				}
			}
		}
	} else if (count == 4) {
		// rect2 在 rect1 内
		rect2.right = rect2.left;
		return true;
	}

	return false;
}

static void OptimizeDirtyRects(SmallVectorImpl<Rect>& dirtyRects) noexcept {
	// 持续循环直到不再能优化
	while (true) {
		const uint32_t count = (uint32_t)dirtyRects.size();
		assert(count > 0);

		bool optimized = false;
		for (uint32_t i = 0; i < count; ++i) {
			for (uint32_t j = i + 1; j < count; ++j) {
				if (OptimizeDirtyRectPair(dirtyRects[i], dirtyRects[j])) {
					optimized = true;
				}
			}
		}

		if (!optimized) {
			return;
		}

		// 从后向前删除空矩形
		for (int i = int(count - 1); i >= 0; --i) {
			const Rect& rect = dirtyRects[i];
			if (RectHelper::IsEmpty(rect)) {
				dirtyRects.erase(dirtyRects.begin() + i);
			}
		}
	}
}

void DirtyRectsOptimizer::Execute(SmallVectorImpl<Rect>& dirtyRects, uint32_t dirtyRectCountLimit) noexcept {
	assert(dirtyRects.size() >= 2);

	OptimizeDirtyRects(dirtyRects);

	while (true) {
		const uint32_t count = (uint32_t)dirtyRects.size();

		uint32_t originTotalPixels = 0;
		for (uint32_t i = 0; i < count; ++i) {
			originTotalPixels += RectHelper::CalcArea(dirtyRects[i]);
		}

		uint32_t minTotalPixels = std::numeric_limits<uint32_t>::max();
		uint32_t targetRectCount = 0;
		bool targetCanOptimize = false;
		uint32_t targetI = 0;
		uint32_t targetJ = 0;
		// 遍历所有两两合并的方式找出总像素数最少的
		for (uint32_t i = 0; i < count; ++i) {
			for (uint32_t j = i + 1; j < count; ++j) {
				const Rect& rect1 = dirtyRects[i];
				const Rect& rect2 = dirtyRects[j];

				// 两个矩形必须相交才有优化的可能，但脏矩形数量过多时需要强制合并
				if (!RectHelper::IsOverlap(rect1, rect2) && count <= dirtyRectCountLimit) {
					continue;
				}

				Rect unionedRect = RectHelper::Union(rect1, rect2);
				uint32_t totalPixels = 0;
				uint32_t rectCount = 0;
				bool optimized = false;

				// 为了降低复杂度这里只优化一轮，而不是调用 OptimizeDirtyRects
				for (uint32_t k = 0; k < count; ++k) {
					if (k == i || k == j) {
						continue;
					}

					Rect curRect = dirtyRects[k];
					if (OptimizeDirtyRectPair(curRect, unionedRect)) {
						optimized = true;
					}

					if (!RectHelper::IsEmpty(curRect)) {
						totalPixels += RectHelper::CalcArea(curRect);
						++rectCount;
					}
				}

				if (!RectHelper::IsEmpty(unionedRect)) {
					totalPixels += RectHelper::CalcArea(unionedRect);
					++rectCount;
				}

				if (totalPixels < minTotalPixels || (totalPixels == minTotalPixels && rectCount < targetRectCount)) {
					minTotalPixels = totalPixels;
					targetRectCount = rectCount;
					targetCanOptimize = optimized;
					targetI = i;
					targetJ = j;
				}
			}
		}

		// 总像素数持平也采用，因为脏矩形数量减少了
		if (minTotalPixels > originTotalPixels && count <= dirtyRectCountLimit) {
			return;
		}

		Rect unionedRect = RectHelper::Union(dirtyRects[targetI], dirtyRects[targetJ]);
		dirtyRects.erase(dirtyRects.begin() + targetJ);
		dirtyRects.erase(dirtyRects.begin() + targetI);
		dirtyRects.push_back(unionedRect);

		if (targetCanOptimize) {
			OptimizeDirtyRects(dirtyRects);

			if (minTotalPixels > originTotalPixels && dirtyRects.size() <= dirtyRectCountLimit) {
				return;
			}
		}
	}
}

#ifdef _DEBUG
static Ignore _ = [] {
	auto rectComp = [](const Rect& l, const Rect& r) {
		return std::tuple(l.left, l.top, l.right, l.bottom) <
			std::tuple(r.left, r.top, r.right, r.bottom);
	};

	SmallVector<Rect, 0> dirtyRects;
	dirtyRects.reserve(16);

	dirtyRects.emplace_back(0, 0, 2, 2);
	dirtyRects.emplace_back(1, 1, 3, 4);
	dirtyRects.emplace_back(2, 1, 4, 3);
	dirtyRects.emplace_back(0, 1, 3, 2);
	dirtyRects.emplace_back(3, 3, 4, 4);

	OptimizeDirtyRects(dirtyRects);

	std::sort(dirtyRects.begin(), dirtyRects.end(), rectComp);
	assert(dirtyRects.size() == 2);
	assert((dirtyRects[0] == Rect{ 0, 0, 2, 2 }));
	assert((dirtyRects[1] == Rect{ 1, 1, 4, 4 }));

	return Ignore();
}();
#endif

}

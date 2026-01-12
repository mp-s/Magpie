#include "pch.h"
#include "CursorDrawer2.h"
#include "ScalingWindow.h"
#include "Win32Helper.h"
#include "Logger.h"
#include <ShellScalingApi.h>
#include <wil/registry.h>

namespace Magpie {

// 系统 DPI 在程序的生命周期内不会改变，而且使用 GetIconInfo 获得的位图尺寸
// 和此值有关。
static UINT SYSTEM_DPI;

static DWORD GetCursorBaseSize() noexcept {
	DWORD cursorBaseSize = 32;

	HRESULT hr = wil::reg::get_value_nothrow(
		HKEY_CURRENT_USER, L"Control Panel\\Cursors", L"CursorBaseSize", &cursorBaseSize);
	// 键不存在不视为错误
	if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
		Logger::Get().ComError("wil::reg::get_value_dword_nothrow 失败", hr);
	}

	return cursorBaseSize;
}

bool CursorDrawer2::Initialize(GraphicsContext& graphicsContext, const RECT& destRect) noexcept {
	_graphicsContext = &graphicsContext;
	_destRect = destRect;

	[[maybe_unused]] static Ignore _ = []() {
		SYSTEM_DPI = GetDpiForSystem();
		return Ignore();
	}();

	wil::unique_hkey key;
	wil::reg::open_unique_key_nothrow(HKEY_CURRENT_USER, L"Control Panel\\Cursors", key);
	_regWatcher = wil::make_registry_watcher_nothrow(std::move(key), false, [this](wil::RegistryChangeKind) {
		ScalingWindow::Dispatcher().TryEnqueue(
			[this, cursorBaseSize(GetCursorBaseSize()), runId(ScalingWindow::RunId())]() {
			if (ScalingWindow::RunId() != runId || _cursorBaseSize == cursorBaseSize) {
				return;
			}

			_cursorBaseSize = cursorBaseSize;
		});
	});

	_cursorBaseSize = GetCursorBaseSize();

	return true;
}

bool CursorDrawer2::CheckForRedraw(HCURSOR& hCursor, POINT cursorPos) noexcept {
	const ScalingWindow& scalingWindow = ScalingWindow::Get();
	const ScalingOptions& options = scalingWindow.Options();

	// 检查自动隐藏光标
	if (options.autoHideCursorDelay.has_value()) {
		using namespace std::chrono;

		// 光标在叠加层上或拖动窗口时禁用自动隐藏。光标处于隐藏状态视为形状不变，考虑形状
		// 变化：箭头->隐藏->箭头，只要位置不变，自动隐藏功能应让光标始终隐藏；反之如果光
		// 标隐藏时移动了或显示时形状变化了应正常显示。
		if (_isCursorVirtualized &&
			!_isMoving &&
			!_isSrcMoving &&
			_curCursorPos == cursorPos &&
			(_lastRawCursorHandle == hCursor || !hCursor)
		) {
			const duration<float> hideDelay(*options.autoHideCursorDelay);
			if (steady_clock::now() - _lastCursorActiveTime > hideDelay) {
				hCursor = NULL;
			}
		} else {
			// 启用自动隐藏时光标形状或位置变化后应记录新的形状、位置和变化时间。位置由
			// _curCursorPos 记录。
			_lastRawCursorHandle = hCursor;
			_lastCursorActiveTime = steady_clock::now();
		}
	}

	if (hCursor) {
		if (_isCursorVisible) {
			// 检查光标是否在视口内
			const _CursorInfo* cursorInfo = _ResolveCursor(hCursor, cursorPos);
			if (cursorInfo) {
				const RECT cursorRect = {
					cursorPos.x,
					cursorPos.y,
					cursorPos.x + (LONG)cursorInfo->size.width,
					cursorPos.y + (LONG)cursorInfo->size.height
				};
				if (!Win32Helper::IsRectOverlap(cursorRect, _destRect)) {
					hCursor = NULL;
				}
			} else {
				Logger::Get().Error("_ResolveCursor 失败");
				hCursor = NULL;
			}
		} else {
			// 截屏时暂时不渲染光标
			hCursor = NULL;
		}
	}

	// 光标形状或位置变化时需要重新绘制
	if (hCursor != _hCurCursor || (hCursor && cursorPos != _curCursorPos)) {
		_hCurCursor = hCursor;
		_curCursorPos = cursorPos;
		return true;
	} else {
		return false;
	}
}

void CursorDrawer2::Draw() noexcept {

}

static Size CalcCursorSize(
	Size cursorBmpSize,
	uint32_t cursorDpi,
	uint32_t monitorDpi,
	uint32_t cursorBaseSize
) noexcept {
	const double scale = (GetSystemMetricsForDpi(SM_CXCURSOR, monitorDpi) * cursorBaseSize) /
		double(GetSystemMetricsForDpi(SM_CXCURSOR, cursorDpi) * 32);
	return { (uint32_t)std::lround(cursorBmpSize.width * scale),
		(uint32_t)std::lround(cursorBmpSize.height * scale) };
}

const CursorDrawer2::_CursorInfo* CursorDrawer2::_ResolveCursor(
	HCURSOR hCursor,
	POINT cursorPos
) noexcept {
	// 检索光标所在屏幕的 DPI
	const HMONITOR hCurMon = MonitorFromPoint(
		{ cursorPos.x + _destRect.left, cursorPos.y + _destRect.top }, MONITOR_DEFAULTTOPRIMARY);
	UINT monitorDpi = USER_DEFAULT_SCREEN_DPI;
	GetDpiForMonitor(hCurMon, MDT_EFFECTIVE_DPI, &monitorDpi, &monitorDpi);
	
	auto it = _cursorInfos.find(std::make_pair(hCursor, monitorDpi));
	if (it != _cursorInfos.end()) {
		return &it->second;
	}

	// 检查此光标是否不随 DPI 缩放
	it = _cursorInfos.find(std::make_pair(hCursor, 0));
	if (it != _cursorInfos.end()) {
		return &it->second;
	}

	ICONINFOEX iconInfoEx = { .cbSize = sizeof(iconInfoEx) };
	if (!GetIconInfoEx(hCursor, &iconInfoEx)) {
		Logger::Get().Win32Error("GetIconInfoEx 失败");
		return nullptr;
	}

	wil::unique_hbitmap hColorBmp(iconInfoEx.hbmColor);
	wil::unique_hbitmap hMaskBmp(iconInfoEx.hbmMask);

	Size bmpSize;
	{
		BITMAP bmp{};
		if (!GetObject(hColorBmp ? hColorBmp.get() : hMaskBmp.get(), sizeof(bmp), &bmp)) {
			Logger::Get().Win32Error("GetObject 失败");
			return nullptr;
		}

		// 单色光标的掩码位图高度是光标实际高度的两倍
		bmpSize = {
			uint32_t(bmp.bmWidth),
			uint32_t(hColorBmp ? bmp.bmHeight : bmp.bmHeight / 2)
		};
	}
	
	_CursorInfo cursorInfo{};
	// 如果不能确定光标是否随 DPI 缩放则假设为真，绝大多数情况下是对的
	bool isCursorDpiAware = true;

	// 将线程 DPI 感知设为 unaware 后 GetIconInfo 可以获得 100% DPI 缩放下的光标位图。
	// 我们借助这个特性检查光标是否随 DPI 缩放，不过只在程序启动时系统 DPI 缩放不是
	// 100% 时有效。
	if (SYSTEM_DPI == 96) {
		cursorInfo.size = CalcCursorSize(bmpSize, SYSTEM_DPI, monitorDpi, _cursorBaseSize);
	} else {
		ICONINFO iconInfoDpi96{};
		{
			DPI_AWARENESS_CONTEXT oldDpiContext =
				SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_UNAWARE);
			auto se = wil::scope_exit([&] {
				SetThreadDpiAwarenessContext(oldDpiContext);
			});

			if (!GetIconInfo(hCursor, &iconInfoDpi96)) {
				Logger::Get().Win32Error("GetIconInfo 失败");
				return nullptr;
			}
		}

		wil::unique_hbitmap hColorBmpDpi96(iconInfoDpi96.hbmColor);
		wil::unique_hbitmap hMaskBmpDpi96(iconInfoDpi96.hbmMask);

		Size bmpSizeDpi96;
		{
			BITMAP bmp{};
			if (!GetObject(hColorBmpDpi96 ? hColorBmpDpi96.get() : hMaskBmpDpi96.get(), sizeof(bmp), &bmp)) {
				Logger::Get().Win32Error("GetObject 失败");
				return nullptr;
			}

			bmpSizeDpi96 = {
				uint32_t(bmp.bmWidth),
				uint32_t(hColorBmpDpi96 ? bmp.bmHeight : bmp.bmHeight / 2)
			};
		}

		// 不同 DPI 下光标位图尺寸不变说明光标不跟随 DPI 缩放
		if (bmpSize == bmpSizeDpi96) {
			isCursorDpiAware = false;
			cursorInfo.size = bmpSizeDpi96;
		} else {
			cursorInfo.size = CalcCursorSize(bmpSizeDpi96, 96, monitorDpi, _cursorBaseSize);
		}

		if (cursorInfo.size == bmpSizeDpi96) {
			hColorBmp = std::move(hColorBmpDpi96);
			hMaskBmp = std::move(hMaskBmpDpi96);
			bmpSize = bmpSizeDpi96;
		}
	}

	cursorInfo.resourceSize = bmpSize;

	return &_cursorInfos.emplace(std::make_pair(hCursor, isCursorDpiAware ? monitorDpi : 0),
		std::move(cursorInfo)).first->second;
}

}

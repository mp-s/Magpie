#include "pch.h"
#include "CursorDrawer2.h"
#include "CursorHelper.h"
#include "ScalingWindow.h"
#include "Win32Helper.h"
#include "Logger.h"
#include <ShellScalingApi.h>
#include <wil/registry.h>

namespace Magpie {

using FnGetCursorFrameInfo = HCURSOR WINAPI(
	HCURSOR hcur,
	LPWSTR  lpName,
	int     iFrame,
	LPDWORD pjifRate,
	LPINT   pccur
);

static FnGetCursorFrameInfo* GetCursorFrameInfo = nullptr;

// 系统 DPI 在程序的生命周期内不会改变，而且使用 GetIconInfo 获得的位图尺寸
// 和此值有关。
static UINT SYSTEM_DPI;

struct StandardCursorInfo {
	HCURSOR handle;
	const wchar_t* regValueName;
	int resId;
};

static std::array<StandardCursorInfo, 16> STANDARD_CURSORS;

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

	[[maybe_unused]] static Ignore _ = [] {
		GetCursorFrameInfo = Win32Helper::LoadFunction<FnGetCursorFrameInfo>(
			L"user32.dll", "GetCursorFrameInfo");

		SYSTEM_DPI = GetDpiForSystem();

		// 不包含已废弃的光标, 见 https://learn.microsoft.com/en-us/windows/win32/menurc/about-cursors
		STANDARD_CURSORS[0] = { LoadCursor(NULL, IDC_ARROW), L"Arrow", 0 };
		STANDARD_CURSORS[1] = { LoadCursor(NULL, IDC_IBEAM), L"IBeam", 0 };
		// Wait 和 AppStarting 在“指针大小” 为 1 时是动态光标，否则是静态光标
		STANDARD_CURSORS[2] = { LoadCursor(NULL, IDC_WAIT), L"Wait", 0 };
		STANDARD_CURSORS[3] = { LoadCursor(NULL, IDC_CROSS), L"Crosshair", 0 };
		STANDARD_CURSORS[4] = { LoadCursor(NULL, IDC_UPARROW), L"UpArrow", 0 };
		STANDARD_CURSORS[5] = { LoadCursor(NULL, IDC_SIZENWSE), L"SizeNWSE", 0 };
		STANDARD_CURSORS[6] = { LoadCursor(NULL, IDC_SIZENESW), L"SizeNESW", 0 };
		STANDARD_CURSORS[7] = { LoadCursor(NULL, IDC_SIZEWE), L"SizeWE", 0 };
		STANDARD_CURSORS[8] = { LoadCursor(NULL, IDC_SIZENS), L"SizeNS", 0 };
		STANDARD_CURSORS[9] = { LoadCursor(NULL, IDC_SIZEALL), L"SizeAll", 0 };
		STANDARD_CURSORS[10] = { LoadCursor(NULL, IDC_NO), L"No", 0 };
		STANDARD_CURSORS[11] = { LoadCursor(NULL, IDC_HAND), L"Hand", 0 };
		STANDARD_CURSORS[12] = { LoadCursor(NULL, IDC_APPSTARTING), L"AppStarting", 0 };
		STANDARD_CURSORS[13] = { LoadCursor(NULL, IDC_HELP), L"Help", 0 };
		// Pin 和 Person 只在“指针大小”选项大于 1 时使用注册表中路径，否则使用
		// user32.dll 中的光标资源，ID 分别是 117 和 118。
		STANDARD_CURSORS[14] = { LoadCursor(NULL, IDC_PIN), L"Pin", 117 };
		STANDARD_CURSORS[15] = { LoadCursor(NULL, IDC_PERSON), L"Person", 118 };

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
			const _CursorInfo* cursorInfo = _ResolveCursor(hCursor, cursorPos, false);
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

static bool GetCursorSizeFromBmps(HBITMAP hColorBmp, HBITMAP hMaskBmp, Size& size) noexcept {
	BITMAP bmp{};
	if (!GetObject(hColorBmp ? hColorBmp : hMaskBmp, sizeof(bmp), &bmp)) {
		Logger::Get().Win32Error("GetObject 失败");
		return false;
	}

	size.width = uint32_t(bmp.bmWidth);
	// 单色光标的掩码位图高度是光标实际高度的两倍
	size.height = uint32_t(hColorBmp ? bmp.bmHeight : bmp.bmHeight / 2);
	return true;
}

const CursorDrawer2::_CursorInfo* CursorDrawer2::_ResolveCursor(
	HCURSOR hCursor,
	POINT cursorPos,
	bool isAni
) noexcept {
	assert(hCursor);

	/*if (GetCursorFrameInfo) {
		DWORD jifRate;
		int stepCount;
		HCURSOR hTmpCursor = GetCursorFrameInfo(hCursor, nullptr, 0, &jifRate, &stepCount);
		if (hTmpCursor && hTmpCursor != hCursor) {
			hCursor = hTmpCursor;
		}
	}*/

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

	_CursorInfo cursorInfo{};
	// 如果不能确定光标是否随 DPI 缩放则假设为真，绝大多数情况下是对的
	bool isCursorDpiAware = !isAni;

	ICONINFOEX iconInfoEx = { .cbSize = sizeof(iconInfoEx) };
	if (!GetIconInfoEx(hCursor, &iconInfoEx)) {
		Logger::Get().Win32Error("GetIconInfoEx 失败");
		return nullptr;
	}

	wil::unique_hbitmap hColorBmp(iconInfoEx.hbmColor);
	wil::unique_hbitmap hMaskBmp(iconInfoEx.hbmMask);

	if (!GetCursorSizeFromBmps(hColorBmp.get(), hMaskBmp.get(), cursorInfo.resourceSize)) {
		Logger::Get().Error("GetCursorSizeFromBmps 失败");
		return nullptr;
	}
	
	// 将线程 DPI 感知设为 unaware 后 GetIconInfo 可以获得 100% DPI 缩放下的光标位图。
	// 我们借助这个特性检查光标是否随 DPI 缩放，不过只在程序启动时系统 DPI 缩放不是
	// 100% 时有效。
	if (SYSTEM_DPI == 96) {
		cursorInfo.size = CalcCursorSize(
			cursorInfo.resourceSize, SYSTEM_DPI, monitorDpi, _cursorBaseSize);
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
		if (!GetCursorSizeFromBmps(hColorBmpDpi96.get(), hMaskBmpDpi96.get(), bmpSizeDpi96)) {
			Logger::Get().Error("GetCursorSizeFromBmps 失败");
			return nullptr;
		}

		// 不同 DPI 下光标位图尺寸不变说明光标不跟随 DPI 缩放
		if (cursorInfo.resourceSize == bmpSizeDpi96) {
			isCursorDpiAware = false;
			cursorInfo.size = bmpSizeDpi96;
		} else {
			cursorInfo.size = CalcCursorSize(bmpSizeDpi96, 96, monitorDpi, _cursorBaseSize);
		}

		if (cursorInfo.size == bmpSizeDpi96) {
			hColorBmp = std::move(hColorBmpDpi96);
			hMaskBmp = std::move(hMaskBmpDpi96);
			cursorInfo.resourceSize = bmpSizeDpi96;
		}
	}

	wil::unique_hcursor hResCursor;

	const auto it1 = std::find_if(
		STANDARD_CURSORS.begin(),
		STANDARD_CURSORS.end(),
		[&](const StandardCursorInfo& info) {
			return info.handle == hCursor;
		}
	);
	if (it1 == STANDARD_CURSORS.end()) {
		hResCursor = _TryResolveCursorResource(iconInfoEx, cursorInfo.size.width);
	} else {
		hResCursor = _TryResolveStandardCursor(it1->regValueName, it1->resId, cursorInfo.size.width);
	}

	if (hResCursor) {
		ICONINFO iconInfo{};
		if (!GetIconInfo(hCursor, &iconInfo)) {
			Logger::Get().Win32Error("GetIconInfo 失败");
			return nullptr;
		}

		hColorBmp.reset(iconInfo.hbmColor);
		hMaskBmp.reset(iconInfo.hbmMask);
		if (!GetCursorSizeFromBmps(hColorBmp.get(), hMaskBmp.get(), cursorInfo.resourceSize)) {
			Logger::Get().Error("GetCursorSizeFromBmps 失败");
			return nullptr;
		}
	}

	return &_cursorInfos.emplace(std::make_pair(hCursor, isCursorDpiAware ? monitorDpi : 0),
		std::move(cursorInfo)).first->second;
}

wil::unique_hcursor CursorDrawer2::_TryResolveCursorResource(
	const ICONINFOEX& iconInfoEx,
	uint32_t preferedWidth
) const noexcept {
	wil::unique_hcursor result;

	if (StrHelper::StrLen(iconInfoEx.szModName) == 0) {
		return result;
	}

	HMODULE hModule = LoadLibraryEx(iconInfoEx.szModName, NULL, LOAD_LIBRARY_AS_DATAFILE);
	if (hModule) {
		LPCWSTR resName = iconInfoEx.wResID != 0 ?
			MAKEINTRESOURCE(iconInfoEx.wResID) : iconInfoEx.szResName;
		result = CursorHelper::ExtractCursorFromModule(hModule, resName, preferedWidth);
		FreeLibrary(hModule);
	} else {
		Logger::Get().Win32Error("LoadLibraryEx 失败");
	}
	
	return result;
}

wil::unique_hcursor CursorDrawer2::_TryResolveStandardCursor(
	const wchar_t* regValueName,
	int resId,
	uint32_t preferedWidth
) const noexcept {
	wil::unique_hcursor result;

	if (_cursorBaseSize == 32 && resId != 0) {
		HMODULE hUser32 = GetModuleHandle(L"user32.dll");
		result = CursorHelper::ExtractCursorFromModule(
			hUser32, MAKEINTRESOURCE(resId), preferedWidth);
		if (!result) {
			Logger::Get().Error("CursorHelper::ExtractCursorFromModule 失败");
		}
	} else {
		wil::unique_bstr regValue;
		HRESULT hr = wil::reg::get_value_nothrow(
			HKEY_CURRENT_USER, L"Control Panel\\Cursors", regValueName, &regValue);
		// 失败不视为错误
		if (FAILED(hr)) {
			return result;
		}

		// 可能为空
		if (SysStringLen(regValue.get()) == 0) {
			return result;
		}

		// 路径中可能包含环境变量字符串
		std::wstring curPath;
		hr = wil::ExpandEnvironmentStringsW(regValue.get(), curPath);
		if (FAILED(hr)) {
			Logger::Get().ComError("wil::ExpandEnvironmentStringsW 失败", hr);
			return result;
		}
		
		result = CursorHelper::ExtractCursorFromCurFile(curPath.c_str(), preferedWidth);
		if (!result) {
			Logger::Get().Error("CursorHelper::ExtractCursorFromCurFile 失败");
		}
	}

	return result;
}

}

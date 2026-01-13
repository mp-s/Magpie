#include "pch.h"
#include "CursorHelper.h"
#include "Logger.h"
#include <SmallVector.h>

namespace Magpie {

bool CursorHelper::GetCursorSize(HBITMAP hColorBmp, HBITMAP hMaskBmp, Size& size) noexcept {
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

wil::unique_hcursor CursorHelper::ExtractCursorFromModule(
	HMODULE hModule,
	LPWSTR resName,
	uint32_t preferredWidth
) noexcept {
	HRSRC hRes = FindResource(hModule, resName, RT_GROUP_CURSOR);
	if (!hRes) {
		Logger::Get().Win32Error("FindResource 失败");
		return nullptr;
	}

	HGLOBAL hResLoad = LoadResource(hModule, hRes);
	if (!hResLoad) {
		Logger::Get().Win32Error("LoadResource 失败");
		return nullptr;
	}

	// 解析光标资源
#pragma pack(push, 2)
	// 来自 https://learn.microsoft.com/en-us/windows/win32/menurc/resdir
	struct RESDIR {
		WORD Width;
		WORD Height;
		WORD Planes;
		WORD BitCount;
		DWORD BytesInRes;
		WORD IconCursorId;
	};

	// 来自 https://learn.microsoft.com/en-us/windows/win32/menurc/newheader
	struct NEWHEADER {
		WORD Reserved;
		WORD ResType;
		WORD ResCount;
		RESDIR entries[1];
	};
#pragma pack(pop)

	const NEWHEADER& header = *(const NEWHEADER*)LockResource(hResLoad);
	if (header.ResType != 2) {
		Logger::Get().Error("不是光标资源");
		return nullptr;
	}

	const uint32_t resCount = header.ResCount;
	if (resCount == 0) {
		Logger::Get().Error("无可用光标资源");
		return nullptr;
	}

	struct IconInfo {
		uint16_t width;
		uint16_t bitCount;
		uint16_t id;
	};
	SmallVector<IconInfo, 0> iconInfos(resCount);

	for (uint32_t i = 0; i < resCount; ++i) {
		const RESDIR& entry = header.entries[i];
		// 宽度和高度的 0 等价于 256
		iconInfos[i] = IconInfo{
			entry.Width == 0 ? (uint16_t)256 : (uint16_t)entry.Width,
			entry.BitCount,
			entry.IconCursorId
		};
	}

	// 尺寸从小到大排序；如果尺寸相同，色深从大到小排序，以便获得色深最大的光标
	std::sort(iconInfos.begin(), iconInfos.end(), [](const IconInfo& l, const IconInfo& r) {
		return l.width < r.width || (l.width == r.width && l.bitCount > r.bitCount);
	});

	// 寻找完美匹配或更大的资源
	uint16_t targetResId;
	{
		auto it1 = std::upper_bound(
			iconInfos.begin(),
			iconInfos.end(),
			preferredWidth,
			[](uint32_t target, const IconInfo& iconInfo) {
				return target < iconInfo.width;
			}
		);
		if (it1 == iconInfos.end()) {
			targetResId = iconInfos.back().id;
		} else {
			targetResId = it1->id;
		}
	}

	hRes = FindResource(hModule, MAKEINTRESOURCE(targetResId), RT_CURSOR);
	if (!hRes) {
		Logger::Get().Win32Error("FindResource 失败");
		return nullptr;
	}

	hResLoad = LoadResource(hModule, hRes);
	if (!hResLoad) {
		Logger::Get().Win32Error("LoadResource 失败");
		return nullptr;
	}

	HICON hIcon = CreateIconFromResourceEx((PBYTE)LockResource(hResLoad),
		SizeofResource(hModule, hRes), FALSE, 0x30000, 0, 0, LR_DEFAULTCOLOR);
	if (!hIcon) {
		Logger::Get().Win32Error("CreateIconFromResourceEx 失败");
		return nullptr;
	}

	return wil::unique_hcursor(hIcon);
}

wil::unique_hcursor CursorHelper::ExtractCursorFromCurFile(
	std::wstring /*fileName*/,
	uint32_t /*preferredWidth*/
) noexcept {
	return wil::unique_hcursor();
}

}

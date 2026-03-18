#include "pch.h"
#include "CursorHelper.h"
#include "Logger.h"
#include "ByteBuffer.h"
#include "SmallVector.h"
#include <mmsystem.h> // FOURCC

namespace Magpie {

struct RTAG {
	DWORD ckID;
	DWORD ckSize;
};

static WORD GetRealIconSize(WORD size) noexcept {
	// 0 等价于 256
	return size == 0 ? (WORD)256 : size;
}

wil::unique_hcursor CursorHelper::ExtractCursorFromModule(
	HMODULE hModule,
	LPCWSTR resName,
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
	if (header.Reserved != 0 || header.ResType != 2) {
		Logger::Get().Error("不是光标资源");
		return nullptr;
	}

	const uint32_t resCount = header.ResCount;
	if (resCount == 0 || resCount > 256) {
		Logger::Get().Error("无可用光标资源");
		return nullptr;
	}

	struct IconInfo {
		WORD width;
		WORD bitCount;
		WORD id;
	};
	SmallVector<IconInfo, 0> iconInfos(resCount);

	for (uint32_t i = 0; i < resCount; ++i) {
		const RESDIR& entry = header.entries[i];
		// 宽度和高度的 0 等价于 256
		iconInfos[i] = IconInfo{
			GetRealIconSize(entry.Width),
			entry.BitCount,
			entry.IconCursorId
		};
	}

	// 尺寸从小到大排序；如果尺寸相同，色深从大到小排序，以便获得色深最大的光标
	std::sort(iconInfos.begin(), iconInfos.end(), [](const IconInfo& l, const IconInfo& r) {
		return l.width < r.width || (l.width == r.width && l.bitCount > r.bitCount);
	});

	// 寻找完美匹配或更大的资源
	WORD targetResId;
	{
		auto it = std::lower_bound(
			iconInfos.begin(),
			iconInfos.end(),
			preferredWidth,
			[](const IconInfo& iconInfo, uint32_t target) {
				return iconInfo.width < target;
			}
		);
		if (it == iconInfos.end()) {
			targetResId = iconInfos.back().id;
		} else {
			targetResId = it->id;
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

// 解析 cur 文件，参考自
// https://learn.microsoft.com/en-us/previous-versions/ms997538(v=msdn.10)
// https://en.wikipedia.org/wiki/ICO_(file_format)#File_structure
static bool LoadIcoFromFileMap(
	const uint8_t* fileData,
	const uint8_t* fileEnd,
	uint32_t preferredWidth,
	SmallVectorImpl<std::pair<wil::unique_hcursor, std::chrono::steady_clock::duration>>& result
) noexcept {
#pragma pack(push, 2)
	struct ICONDIR {
		WORD idReserved;
		WORD idType;
		WORD idCount;
	};

	struct ICONDIRENTRY {
		BYTE bWidth;
		BYTE bHeight;
		BYTE bColorCount;
		BYTE bReserved;
		WORD xHotSpot;
		WORD yHotSpot;
		DWORD dwBytesInRes;
		DWORD dwImageOffset;
	};

	struct LOCALHEADER {
		WORD xHotSpot;
		WORD yHotSpot;
	};
#pragma pack(pop)

	if (fileData + sizeof(ICONDIR) > fileEnd) {
		Logger::Get().Error("文件无效");
		return false;
	}

	uint32_t entryCount;
	{
		const ICONDIR& header = *(ICONDIR*)fileData;

		if (header.idReserved != 0 || header.idType != 2) {
			Logger::Get().Error("不是光标资源");
			return false;
		}

		if (header.idCount == 0 || header.idCount > 256) {
			Logger::Get().Error("无可用光标资源");
			return false;
		}

		entryCount = header.idCount;
	}

	const ICONDIRENTRY* pEntries = (const ICONDIRENTRY*)(fileData + sizeof(ICONDIR));

	if ((uint8_t*)pEntries + sizeof(ICONDIRENTRY) * entryCount > fileEnd) {
		Logger::Get().Error("文件无效");
		return false;
	}

	// 寻找完美匹配或更大的资源
	const ICONDIRENTRY* targetEntry;
	{
		std::vector<const ICONDIRENTRY*> entries(entryCount);

		for (uint32_t i = 0; i < entryCount; ++i) {
			entries[i] = &pEntries[i];
		}

		// 尺寸从小到大排序；和资源不同，cur 文件不区分色深
		std::sort(entries.begin(), entries.end(), [](const ICONDIRENTRY* l, const ICONDIRENTRY* r) {
			return GetRealIconSize(l->bWidth) < GetRealIconSize(r->bWidth);
		});

		auto it = std::lower_bound(
			entries.begin(),
			entries.end(),
			preferredWidth,
			[](const ICONDIRENTRY* entry, uint32_t target) {
				return GetRealIconSize(entry->bWidth) < target;
			}
		);
		if (it == entries.end()) {
			targetEntry = entries.back();
		} else {
			targetEntry = *it;
		}
	}

	const uint8_t* pCursroData = fileData + targetEntry->dwImageOffset;

	if (pCursroData + targetEntry->dwBytesInRes > fileEnd) {
		Logger::Get().Error("文件无效");
		return false;
	}

	// RT_CURSOR 结构为 LOCALHEADER 后跟位图数据
	// https://learn.microsoft.com/en-us/windows/win32/menurc/resource-file-formats#cursor-and-icon-resources
	ByteBuffer cursorData(sizeof(LOCALHEADER) + targetEntry->dwBytesInRes);
	// 设置热点
	*(LOCALHEADER*)cursorData.Data() = { targetEntry->xHotSpot, targetEntry->yHotSpot };
	// 读取位图数据
	std::memcpy(cursorData.Data() + sizeof(LOCALHEADER), pCursroData, targetEntry->dwBytesInRes);

	wil::unique_hcursor hCursor(CreateIconFromResourceEx(cursorData.Data(),
		sizeof(LOCALHEADER) + targetEntry->dwBytesInRes, FALSE, 0x30000, 0, 0, LR_DEFAULTCOLOR));
	if (!hCursor) {
		Logger::Get().Win32Error("CreateIconFromResourceEx 失败");
		return false;
	}

	result.emplace_back(std::move(hCursor), std::chrono::steady_clock::duration::max());
	return true;
}

static bool LoadAniFromFileMap(
	const uint8_t* fileData,
	const uint8_t* fileEnd,
	uint32_t preferredWidth,
	SmallVectorImpl<std::pair<wil::unique_hcursor, std::chrono::steady_clock::duration>>& frames,
	SmallVectorImpl<uint32_t>& frameSequence
) {

#define FOURCC_ACON mmioFOURCC('A', 'C', 'O', 'N')
	return true;
}

bool CursorHelper::ExtractCursorFramesFromFile(
	const wchar_t* fileName,
	uint32_t preferredWidth,
	SmallVectorImpl<std::pair<wil::unique_hcursor, std::chrono::steady_clock::duration>>& frames,
	SmallVectorImpl<uint32_t>& frameSequence
) noexcept {
	assert(frames.empty() && frameSequence.empty());

	CREATEFILE2_EXTENDED_PARAMETERS extendedParams{
		.dwSize = sizeof(CREATEFILE2_EXTENDED_PARAMETERS),
		.dwFileAttributes = FILE_ATTRIBUTE_NORMAL,
		.dwFileFlags = FILE_FLAG_SEQUENTIAL_SCAN
	};
	wil::unique_hfile hFile(CreateFile2(
		fileName, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, &extendedParams));
	if (!hFile) {
		Logger::Get().Win32Error("CreateFile2 失败");
		return false;
	}

	const DWORD fileSize = GetFileSize(hFile.get(), nullptr);
	// 这个检查确保可以访问 RIFF 头，也确保不会把空文件传给 CreateFileMapping
	if (fileSize < sizeof(RTAG)) {
		Logger::Get().Error("文件无效");
		return false;
	}

	wil::unique_handle hFileMap(CreateFileMapping(
		hFile.get(), nullptr, PAGE_READONLY, 0, 0, nullptr));
	if (!hFileMap) {
		Logger::Get().Win32Error("CreateFileMapping 失败");
		return false;
	}

	wil::unique_mapview_ptr<const uint8_t> fileData((const uint8_t*)MapViewOfFile(
		hFileMap.get(), FILE_MAP_READ, 0, 0, 0));
	if (!fileData) {
		Logger::Get().Win32Error("MapViewOfFile 失败");
		return false;
	}

	const uint8_t* fileEnd = fileData.get() + fileSize;

	// 存在 RIFF 头则 ani，否则为 ico
	if (((RTAG*)fileData.get())->ckID == FOURCC_RIFF) {
		if (!LoadAniFromFileMap(fileData.get(), fileEnd, preferredWidth, frames, frameSequence)) {
			Logger::Get().Error("LoadAniFromFileMap 失败");
			return false;
		}
	} else {
		if (!LoadIcoFromFileMap(fileData.get(), fileEnd, preferredWidth, frames)) {
			Logger::Get().Error("LoadIcoFromFileMap 失败");
			return false;
		}
	}

	return true;
}

}

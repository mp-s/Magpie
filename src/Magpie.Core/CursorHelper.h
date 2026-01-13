#pragma once

namespace Magpie {

struct CursorHelper {
	static bool GetCursorSize(HBITMAP hColorBmp, HBITMAP hMaskBmp, Size& size) noexcept;

	// 如果没有完美匹配则倾向于提取较大的资源
	static wil::unique_hcursor ExtractCursorFromModule(
		HMODULE hModule,
		LPWSTR resName,
		uint32_t preferredWidth
	) noexcept;

	static wil::unique_hcursor ExtractCursorFromCurFile(
		std::wstring fileName,
		uint32_t preferredWidth
	) noexcept;
};

}

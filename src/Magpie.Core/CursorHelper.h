#pragma once

namespace Magpie {

struct CursorHelper {
	// 如果没有完美匹配则倾向于提取较大的资源
	static wil::unique_hcursor ExtractCursorFromModule(
		HMODULE hModule,
		LPCWSTR resName,
		uint32_t preferredWidth
	) noexcept;

	static wil::unique_hcursor ExtractCursorFromCurFile(
		const wchar_t* fileName,
		uint32_t preferredWidth
	) noexcept;
};

}

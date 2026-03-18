#pragma once
#include "SmallVector.h"

namespace Magpie {

struct CursorHelper {
	// 如果没有完美匹配则倾向于提取较大的资源
	static wil::unique_hcursor ExtractCursorFromModule(
		HMODULE hModule,
		LPCWSTR resName,
		uint32_t preferredWidth
	) noexcept;

	// 支持 .ico 和 .ani
	static bool ExtractCursorFramesFromFile(
		const wchar_t* fileName,
		uint32_t preferredWidth,
		SmallVectorImpl<std::pair<wil::unique_hcursor, std::chrono::steady_clock::duration>>& result,
		SmallVectorImpl<uint32_t>& frameSequence
	) noexcept;
};

}

#include "pch.h"
#include "ShaderEffectParser.h"
#include "EffectInfo.h"
#include "Win32Helper.h"
#include "ShaderEffectDesc.h"

namespace Magpie {

// 当前 MagpieFX 版本
// static constexpr uint32_t MAGPIE_FX_VERSION = 4;

// static const char* META_INDICATOR = "//!";

class PassInclude : public ID3DInclude {
public:
	PassInclude(std::wstring_view localDir) : _localDir(localDir) {}

	PassInclude(const PassInclude&) = default;
	PassInclude(PassInclude&&) = default;

	HRESULT CALLBACK Open(
		D3D_INCLUDE_TYPE /*IncludeType*/,
		LPCSTR pFileName,
		LPCVOID /*pParentData*/,
		LPCVOID* ppData,
		UINT* pBytes
	) noexcept override {
		std::wstring relativePath = StrHelper::Concat(_localDir, StrHelper::UTF8ToUTF16(pFileName));

		std::string content;
		if (!Win32Helper::ReadTextFile(relativePath.c_str(), content)) {
			Logger::Get().Win32Error("Win32Helper::ReadTextFile 失败");
			return E_FAIL;
		}

		std::unique_ptr<char[]> data = std::make_unique<char[]>(content.size());
		std::memcpy(data.get(), content.data(), content.size());

		*ppData = data.release();
		*pBytes = (UINT)content.size();
		return S_OK;
	}

	HRESULT CALLBACK Close(LPCVOID pData) noexcept override {
		std::unique_ptr<char[]>((char*)pData).reset();
		return S_OK;
	}

private:
	std::wstring _localDir;
};

bool ShaderEffectParser::ParseForInfo(std::string&& name, std::string&& /*source*/, EffectInfo& effectInfo) noexcept {
	effectInfo.name = std::move(name);

	return false;
}

bool ShaderEffectParser::ParseForDesc(
	std::string&& name,
	std::string&& /*source*/,
	std::string&& workingFolder,
	const ShaderEffectParserOptions& /*options*/,
	struct ShaderEffectDesc& effectDesc,
	ShaderEffectSource& effectSource
) noexcept {
	effectDesc.name = std::move(name);
	effectSource.workingFolder = std::move(workingFolder);
	return false;
}

}

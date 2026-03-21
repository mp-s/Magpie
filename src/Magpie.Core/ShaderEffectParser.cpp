#include "pch.h"
#include "ShaderEffectParser.h"
#include "EffectInfo.h"
#include "StrHelper.h"
#include "Logger.h"
#include "ShaderEffectDesc.h"

namespace Magpie {

// 当前 MagpieFX 版本
// static constexpr uint32_t MAGPIE_FX_VERSION = 4;

// 必须出现在一行的开头才视为指令
static const char* META_INDICATOR = "//!";

struct ParserState {
	std::string errorMsg;
	uint32_t lineNumber;
	bool isNewLine;
};

static void RemoveLeadingSpaces(std::string_view& source, ParserState& state) noexcept {
	size_t i = 0;
	for (; i < source.size(); ++i) {
		if (source[i] != ' ') {
			break;
		}
	}

	if (i != 0) {
		source.remove_prefix(i);
		state.isNewLine = false;
	}
}

static bool RemoveLeadingBlanks(std::string_view& source, ParserState& state) noexcept {
	size_t i = 0;
	for (; i < source.size(); ++i) {
		char c = source[i];

		if (c == ' ' || c == '\t') {
			state.isNewLine = false;
			continue;
		}

		if (c == '\n') {
			++state.lineNumber;
			state.isNewLine = true;
			continue;
		}

		if (c == '/') {
			// source 以换行符结尾，因此可以安全提取非换行符的下一个的字符
			char nextChar = source[i + 1];

			if (nextChar == '/') {
				if (!state.isNewLine || source[i + 2] != '!') {
					// 行注释而非指令
					i += 2;
					while (source[i] != '\n') {
						++i;
					}

					++state.lineNumber;
					state.isNewLine = true;
					continue;
				}
			} else if (nextChar == '*') {
				// 块注释
				i += 2;

				while (true) {
					c = source[i];

					if (c == '*') {
						if (source[i + 1] == '/') {
							// 块注释结束
							++i;
							state.isNewLine = false;
							break;
						}
					} else if (c == '\n') {
						// 块注释中换行不记为新行
						++state.lineNumber;

						// 提取换行符的后一个字符需检查文件结尾
						if (i + 1 == source.size()) {
							state.errorMsg = "块注释未闭合";
							return false;
						}
					}

					++i;
				}

				continue;
			}
		}

		break;
	}

	source.remove_prefix(i);
	return true;
}

template <bool SkipBlanks>
static bool GetNextToken(std::string_view& source, ParserState& state, std::string_view& result) noexcept {
	if (SkipBlanks) {
		if (!RemoveLeadingBlanks(source, state)) {
			return false;
		}
	} else {
		RemoveLeadingSpaces(source, state);
	}
	
	if (source.empty()) {
		result = source;
		return true;
	}

	char c = source[0];

	// 必须以字母或下划线开头
	if (!StrHelper::isalpha(c) && c != '_') {
		state.errorMsg = fmt::format("Unexpected character \"{}\" in line {}.", c, state.lineNumber);
		return false;
	}

	// 可以包含字母、数字或下划线
	size_t i = 1;
	for (; i < source.size(); ++i) {
		c = source[i];

		if (!StrHelper::isalnum(c) && c != '_') {
			break;
		}
	}

	result = source.substr(0, i);
	source.remove_prefix(i);
	state.isNewLine = false;
	return true;
}

template <bool SkipBlanks>
static bool CheckNextToken(std::string_view& source, ParserState& state, std::string_view expectedToken) noexcept {
	std::string_view token;
	if (!GetNextToken<SkipBlanks>(source, state, token)) {
		return false;
	}

	if (token == expectedToken) {
		return true;
	} else {
		state.errorMsg = fmt::format("Unexpected token \"{}\" in line {}.", token, state.lineNumber);
		return false;
	}
}

static bool CheckMetaIndicator(std::string_view& source, ParserState& state) noexcept {
	if (!RemoveLeadingBlanks(source, state)) {
		return false;
	}

	if (state.isNewLine && source.starts_with(META_INDICATOR)) {
		source.remove_prefix(StrHelper::StrLen(META_INDICATOR));
		state.isNewLine = false;
		return true;
	} else {
		state.errorMsg = fmt::format("Unexpected character \"{}\" in line {}.", source[0], state.lineNumber);
		return false;
	}
}

static bool CheckLineEnd(std::string_view& source, ParserState& state) noexcept {
	RemoveLeadingSpaces(source, state);

	if (source.empty() || source[0] == '\n') {
		return true;
	} else {
		state.errorMsg = fmt::format("Unexpected character \"{}\" in line {}.", source[0], state.lineNumber);
		return false;
	}
}

static bool CheckMagic(std::string_view& source, ParserState& state) noexcept {
	if (!CheckMetaIndicator(source, state)) {
		return false;
	}

	if (!CheckNextToken<false>(source, state, "MAGPIE")) {
		return false;
	}
	if (!CheckNextToken<false>(source, state, "EFFECT")) {
		return false;
	}

	return CheckLineEnd(source, state);
}

std::string ShaderEffectParser::ParseForInfo(
	std::string&& name,
	std::string&& source,
	EffectInfo2& effectInfo
) noexcept {
	assert(!name.empty() && !source.empty());

	effectInfo.name = std::move(name);

	ParserState state = {
		.lineNumber = 1,
		.isNewLine = true
	};

	// 确保源代码以换行结尾，这可以有效简化文件结尾检查
	if (!source.ends_with('\n')) {
		source.push_back('\n');
	}
	std::string_view sourceView(source);

	if (!CheckMagic(sourceView, state)) {
		Logger::Get().Error("检查 MagpieFX 头失败");
		return std::move(state.errorMsg);
	}

	return std::move(state.errorMsg);
}

bool ShaderEffectParser::ParseForDesc(
	std::string&& name,
	std::string_view source,
	std::string&& workingFolder,
	const ShaderEffectParserOptions& /*options*/,
	struct ShaderEffectDesc& effectDesc,
	ShaderEffectSource& effectSource
) noexcept {
	assert(!name.empty() && !source.empty() && !workingFolder.empty());

	effectDesc.name = std::move(name);
	effectSource.workingFolder = std::move(workingFolder);
	return false;
}

}

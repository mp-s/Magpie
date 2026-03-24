#include "pch.h"
#include "ShaderEffectParser.h"
#include "EffectInfo.h"
#include "StrHelper.h"
#include "Logger.h"
#include "ShaderEffectDesc.h"
#include <bitset>

namespace Magpie {

// 当前 MagpieFX 版本
static constexpr uint32_t MAGPIE_FX_VERSION = 5;

// 必须出现在一行的开头才视为指令
static const char* META_INDICATOR = "//!";

struct ParserState {
	std::string errorMsg;
	uint32_t lineNumber;
	bool isNewLine;
};

static bool RemoveLeadingComment(
	std::string_view& source,
	ParserState& state,
	size_t& i,
	bool& isComment,
	bool& isMetaIdicator
) noexcept {
	assert(source[i] == '/');

	isComment = false;
	isMetaIdicator = false;

	// source 以换行符结尾，因此可以安全提取非换行符的下一个的字符
	char nextChar = source[i + 1];

	if (nextChar == '/') {
		if (state.isNewLine && source[i + 2] == '!') {
			// 指令
			isMetaIdicator = true;
		} else {
			// 行注释
			isComment = true;

			i += 2;
			while (source[i] != '\n') {
				++i;
			}

			++state.lineNumber;
			state.isNewLine = true;
		}
	} else if (nextChar == '*') {
		// 块注释
		isComment = true;
		i += 2;

		while (true) {
			char c = source[i];

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
	}

	return true;
}

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
		} else if (c == '\n') {
			++state.lineNumber;
			state.isNewLine = true;
		} else {
			if (c == '/') {
				bool isComment;
				bool isMetaIdicator;
				if (!RemoveLeadingComment(source, state, i, isComment, isMetaIdicator)) {
					return false;
				}

				if (isComment) {
					continue;
				}
			}

			break;
		}
	}

	source.remove_prefix(i);
	return true;
}

template <bool SkipBlanks, bool AllowEmpty>
static bool GetNextToken(std::string_view& source, ParserState& state, std::string_view& result) noexcept {
	if (SkipBlanks) {
		if (!RemoveLeadingBlanks(source, state)) {
			return false;
		}
	} else {
		RemoveLeadingSpaces(source, state);
	}
	
	if (source.empty()) {
		if constexpr (AllowEmpty) {
			result = source;
			return true;
		} else {
			return false;
		}
	}

	char c = source[0];

	// 必须以字母或下划线开头
	if (!StrHelper::isalpha(c) && c != '_') {
		state.errorMsg = fmt::format("Unexpected character \"{}\" in line {}.", c, state.lineNumber);
		return false;
	}

	// 可以包含字母、数字或下划线
	size_t i = 1;
	while(true) {
		c = source[i];

		if (!StrHelper::isalnum(c) && c != '_') {
			break;
		}

		// source 以换行符结尾，因此无需检查越界
		++i;
	}

	result = source.substr(0, i);
	source.remove_prefix(i);
	state.isNewLine = false;
	return true;
}

template <bool SkipBlanks>
static bool CheckNextToken(std::string_view& source, ParserState& state, std::string_view expectedToken) noexcept {
	assert(!expectedToken.empty());

	std::string_view token;
	if (!GetNextToken<SkipBlanks, false>(source, state, token)) {
		return false;
	}

	if (token == expectedToken) {
		return true;
	} else {
		state.errorMsg = fmt::format("Unexpected token \"{}\" in line {}.", token, state.lineNumber);
		return false;
	}
}

static bool CheckMetaIndicator(std::string_view& source, ParserState& state, bool& result) noexcept {
	if (!RemoveLeadingBlanks(source, state)) {
		return false;
	}

	result = state.isNewLine && source.starts_with(META_INDICATOR);
	if (result) {
		source.remove_prefix(StrHelper::StrLen(META_INDICATOR));
		state.isNewLine = false;
	}

	return true;
}

static bool RequireMetaIndicator(std::string_view& source, ParserState& state) noexcept {
	bool result = false;
	if (!CheckMetaIndicator(source, state, result)) {
		return false;
	}

	if (!result) {
		state.errorMsg = fmt::format("Unexpected character \"{}\" in line {}.", source[0], state.lineNumber);
	}

	return result;
}

static bool RequireLineEnd(std::string_view& source, ParserState& state) noexcept {
	RemoveLeadingSpaces(source, state);

	if (source.empty() || source[0] == '\n') {
		return true;
	} else {
		state.errorMsg = fmt::format("Unexpected character \"{}\" in line {}.", source[0], state.lineNumber);
		return false;
	}
}

static bool CheckMagic(std::string_view& source, ParserState& state) noexcept {
	if (!RequireMetaIndicator(source, state)) {
		return false;
	}

	if (!CheckNextToken<false>(source, state, "MAGPIE")) {
		return false;
	}
	if (!CheckNextToken<false>(source, state, "EFFECT")) {
		return false;
	}

	return RequireLineEnd(source, state);
}

template <bool AllowEmpty>
static bool GetNextStringUntilLineEnd(
	std::string_view& source,
	ParserState& state,
	std::string_view& value
) noexcept {
	assert(value.empty());

	RemoveLeadingSpaces(source, state);

	if constexpr (AllowEmpty) {
		if (source.empty()) {
			return true;
		}
	}

	size_t pos = source.find('\n');

	value = source.substr(0, pos);
	StrHelper::Trim(value);

	if constexpr (!AllowEmpty) {
		if (value.empty()) {
			return false;
		}
	}

	// 源码的最后一个字符必为换行，允许空字符串时已检查 source 不为空，不允许空字符串
	// 时已检查 value 不为空。
	assert(pos != std::string_view::npos);
	// 不删除换行符
	source.remove_prefix(pos);
	state.isNewLine = false;

	return true;
}

template <typename T>
static bool GetNextNumber(std::string_view& source, ParserState& state, T& value) noexcept {
	RemoveLeadingSpaces(source, state);

	const auto& result = std::from_chars(source.data(), source.data() + source.size(), value);
	if ((int)result.ec) {
		return false;
	}

	source.remove_prefix(result.ptr - source.data());
	state.isNewLine = false;
	return true;
}

struct CommandInfo {
	const char* name;
	bool (*resolver)(std::string_view&, ParserState&, void*) noexcept;
	bool isRequired;
};

static bool ResolveBlockCommon(
	std::span<const CommandInfo> commandInfos,
	std::string_view& source,
	ParserState& state,
	void* data
) noexcept {
	SmallVector<bool, 24> processed(commandInfos.size());

	std::string_view token;

	while (true) {
		bool isMetaIndicator = false;
		if (!CheckMetaIndicator(source, state, isMetaIndicator)) {
			return false;
		}

		if (!isMetaIndicator) {
			break;
		}

		if (!GetNextToken<false, false>(source, state, token)) {
			return false;
		}
		std::string command = StrHelper::ToUpperCase(token);

		auto it = std::find_if(commandInfos.begin(), commandInfos.end(), [&](const auto& commandInfo) {
			return commandInfo.name == command;
		});
		if (it != commandInfos.end()) {
			size_t idx = it - commandInfos.begin();

			if (processed[idx]) {
				return false;
			}
			processed[idx] = true;

			if (!it->resolver(source, state, data)) {
				return false;
			}
		} else {
			Logger::Get().Warn(StrHelper::Concat("未知指令: ", token));

			std::string_view unused;
			if (!GetNextStringUntilLineEnd<true>(source, state, unused)) {
				return false;
			}
		}
	}

	// 检查必需项
	for (uint32_t i = 0; i < commandInfos.size(); ++i) {
		if (commandInfos[i].isRequired && !processed[i]) {
			return false;
		}
	}

	return true;
}

static bool ResolveHeaderVersion(
	std::string_view& source,
	ParserState& state,
	void*
) noexcept {
	uint32_t version;
	if (!GetNextNumber(source, state, version)) {
		return false;
	}

	// 向后兼容到 4
	if (version < 4 || version > MAGPIE_FX_VERSION) {
		return false;
	}

	if (!RequireLineEnd(source, state)) {
		return false;
	}

	return true;
}

static bool ResolveHeaderSortName(
	std::string_view& source,
	ParserState& state,
	void* data
) noexcept {
	std::string_view sortName;
	if (!GetNextStringUntilLineEnd<false>(source, state, sortName)) {
		return false;
	}

	((EffectInfo2*)data)->sortName = sortName;
	return true;
}

static bool ResolveHeaderUse(
	std::string_view& source,
	ParserState& state,
	void*
) noexcept {
	std::string_view flags;
	return GetNextStringUntilLineEnd<false>(source, state, flags);
}

static bool ResolveHeaderCapability(
	std::string_view& source,
	ParserState& state,
	void* data
) noexcept {
	std::string_view flags;
	if (!GetNextStringUntilLineEnd<false>(source, state, flags)) {
		return false;
	}

	static constexpr std::array FLAG_INFOS = {
		// 以下为必需
		std::make_pair("FP16", EffectInfoFlags2::SupportFP16),
		// 以下为可选
		std::make_pair("ADVANCEDCOLOR", EffectInfoFlags2::SupportAdvancedColor)
	};

	std::bitset<FLAG_INFOS.size()> processed;

	for (std::string_view& token : StrHelper::Split(flags, ',')) {
		StrHelper::Trim(token);
		std::string flag = StrHelper::ToUpperCase(token);

		auto it = std::find_if(FLAG_INFOS.begin(), FLAG_INFOS.end(), [&](const auto& flagInfo) {
			return flagInfo.first == flag;
		});
		if (it != FLAG_INFOS.end()) {
			size_t idx = it - FLAG_INFOS.begin();

			if (processed[idx]) {
				return false;
			}
			processed[idx] = true;

			((EffectInfo2*)data)->flags |= it->second;
		} else {
			Logger::Get().Warn(StrHelper::Concat("使用了未知 CAPABILITY 标志: ", token));
		}
	}

	return true;
}

static bool ResolveHeader(
	std::string_view source,
	uint32_t startLineNumer,
	EffectInfo2& effectInfo
) noexcept {
	static constexpr std::array COMMAND_INFOS = {
		CommandInfo{ "VERSION", ResolveHeaderVersion, true },
		CommandInfo{ "SORT_NAME", ResolveHeaderSortName, false },
		CommandInfo{ "USE", ResolveHeaderUse, false },
		CommandInfo{ "CAPABILITY", ResolveHeaderCapability, false }
	};

	ParserState state = {
		.lineNumber = startLineNumer,
		.isNewLine = false
	};

	if (!ResolveBlockCommon(COMMAND_INFOS, source, state, &effectInfo)) {
		return false;
	}

	if (!RemoveLeadingBlanks(source, state)) {
		return false;
	}

	if (source.empty()) {
		return true;
	}

	// HEADER 只能有 #include
	if (!source.starts_with("#include")) {
		return false;
	}

	// 跳过整行
	std::string_view unused;
	if (!GetNextStringUntilLineEnd<false>(source, state, unused)) {
		return false;
	}

	// 之后不允许有内容
	if (!RemoveLeadingBlanks(source, state)) {
		return false;
	}

	return source.empty();
}

static bool ResolveParameterDefault(
	std::string_view& source,
	ParserState& state,
	void* data
) noexcept {
	if (!GetNextNumber(source, state, ((EffectInfoParameter*)data)->defaultValue)) {
		return false;
	}

	if (!RequireLineEnd(source, state)) {
		return false;
	}

	return true;
}

static bool ResolveParameterMin(
	std::string_view& source,
	ParserState& state,
	void* data
) noexcept {
	if (!GetNextNumber(source, state, ((EffectInfoParameter*)data)->minValue)) {
		return false;
	}

	if (!RequireLineEnd(source, state)) {
		return false;
	}

	return true;
}

static bool ResolveParameterMax(
	std::string_view& source,
	ParserState& state,
	void* data
) noexcept {
	if (!GetNextNumber(source, state, ((EffectInfoParameter*)data)->maxValue)) {
		return false;
	}

	if (!RequireLineEnd(source, state)) {
		return false;
	}

	return true;
}

static bool ResolveParameterStep(
	std::string_view& source,
	ParserState& state,
	void* data
) noexcept {
	if (!GetNextNumber(source, state, ((EffectInfoParameter*)data)->step)) {
		return false;
	}

	if (!RequireLineEnd(source, state)) {
		return false;
	}

	return true;
}

static bool ResolveParameterLabel(
	std::string_view& source,
	ParserState& state,
	void* data
) noexcept {
	std::string_view label;
	if (!GetNextStringUntilLineEnd<false>(source, state, label)) {
		return false;
	}

	((EffectInfoParameter*)data)->label = label;
	return true;
}

static bool ResolveParameter(
	std::string_view source,
	uint32_t startLineNumer,
	EffectInfoParameter& effectInfoParameter
) noexcept {
	static constexpr std::array COMMAND_INFOS = {
		CommandInfo{ "DEFAULT", ResolveParameterDefault, true },
		CommandInfo{ "MIN", ResolveParameterMin, true },
		CommandInfo{ "MAX", ResolveParameterMax, true },
		CommandInfo{ "STEP", ResolveParameterStep, true },
		CommandInfo{ "LABEL", ResolveParameterLabel, false }
	};

	ParserState state = {
		.lineNumber = startLineNumer,
		.isNewLine = false
	};

	if (!ResolveBlockCommon(COMMAND_INFOS, source, state, &effectInfoParameter)) {
		return false;
	}

	if (effectInfoParameter.minValue > effectInfoParameter.defaultValue ||
		effectInfoParameter.maxValue < effectInfoParameter.defaultValue) {
		return false;
	}

	// 代码部分
	std::string_view token;
	if (!GetNextToken<true, false>(source, state, token)) {
		return false;
	}

	if (token != "float" && token != "int") {
		return false;
	}

	if (!GetNextToken<true, false>(source, state, token)) {
		return false;
	}

	effectInfoParameter.name = token;

	if (!RemoveLeadingBlanks(source, state) || !source.starts_with(';')) {
		return false;
	}

	source.remove_prefix(1);
	state.isNewLine = false;

	// 之后不允许有内容
	if (!RemoveLeadingBlanks(source, state)) {
		return false;
	}

	return source.empty();
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

	enum class BlockType {
		Header,
		Parameter,
		Texture,
		Sampler,
		Common,
		Pass
	};

	struct BlockData {
		std::string_view source;
		uint32_t startLineNumer;
	};

	BlockData headerBlock{};
	SmallVector<BlockData> paramBlocks;

	BlockType curBlockType = BlockType::Header;
	size_t curBlockOffset = 0;
	uint32_t curBlockStartLineNumber = state.lineNumber;

	const auto completeCurrentBlock = [&](
		BlockType newBlockType,
		size_t curBlockEnd,
		size_t newBlockOffset
	) {
		assert(curBlockOffset < curBlockEnd && curBlockEnd < newBlockOffset);
		assert(sourceView[curBlockEnd - 1] == '\n');

		switch (curBlockType) {
		case BlockType::Header:
			headerBlock = BlockData{
				sourceView.substr(curBlockOffset, curBlockEnd - curBlockOffset),curBlockStartLineNumber };
			break;
		case BlockType::Parameter:
			paramBlocks.push_back(BlockData{
				sourceView.substr(curBlockOffset, curBlockEnd - curBlockOffset),curBlockStartLineNumber });
			break;
		case BlockType::Texture:
		case BlockType::Sampler:
		case BlockType::Common:
		case BlockType::Pass:
			break;
		default:
			assert(false);
			break;
		}

		curBlockType = newBlockType;
		curBlockOffset = newBlockOffset;
		curBlockStartLineNumber = state.lineNumber;
	};

	for (size_t i = 0; i < sourceView.size(); ++i) {
		char c = sourceView[i];

		if (c == '\n') {
			++state.lineNumber;
			state.isNewLine = true;
		} else if (c == '/') {
			bool isComment;
			bool isMetaIdicator;
			if (!RemoveLeadingComment(sourceView, state, i, isComment, isMetaIdicator)) {
				return std::move(state.errorMsg);
			}

			if (isMetaIdicator) {
				std::string_view tempSource = sourceView.substr(i + 3);
				std::string_view token;
				if (!GetNextToken<false, false>(tempSource, state, token)) {
					return std::move(state.errorMsg);
				}

				std::string newBlockType = StrHelper::ToUpperCase(token);
				size_t newBlockOffset = tempSource.data() - sourceView.data();

				// sourceView[i - 1] 是换行符，因此每个区块都以换行结尾。区块开头不包含声明该区块的
				// 指令，如果该指令没有参数，此区块就以换行符开头。
				if (newBlockType == "PARAMETER") {
					completeCurrentBlock(BlockType::Parameter, i, newBlockOffset);
				} else if (newBlockType == "TEXTURE") {
					completeCurrentBlock(BlockType::Texture, i, newBlockOffset);
				} else if (newBlockType == "SAMPLER") {
					completeCurrentBlock(BlockType::Sampler, i, newBlockOffset);
				} else if (newBlockType == "COMMON") {
					completeCurrentBlock(BlockType::Common, i, newBlockOffset);
				} else if (newBlockType == "PASS") {
					completeCurrentBlock(BlockType::Pass, i, newBlockOffset);
				}

				i = newBlockOffset;
				state.isNewLine = false;
			}
		}
	}

	// 结束最后一个区块。source 以换行符结尾，因此最后一个区块也以换行符结尾。
	completeCurrentBlock(BlockType::Header, sourceView.size(), std::numeric_limits<size_t>::max());

	if (!ResolveHeader(headerBlock.source, headerBlock.startLineNumer, effectInfo)) {
		Logger::Get().Error(StrHelper::Concat("解析 Header 块失败: ", state.errorMsg));
		return std::move(state.errorMsg);
	}

	effectInfo.params.resize(paramBlocks.size());
	for (size_t i = 0; i < paramBlocks.size(); ++i) {
		if (!ResolveParameter(paramBlocks[i].source, paramBlocks[i].startLineNumer, effectInfo.params[i])) {
			Logger::Get().Error(fmt::format("解析 Parameter#{} 块失败", i + 1));
			return std::move(state.errorMsg);
		}
	}

	return std::move(state.errorMsg);
}

bool ShaderEffectParser::ParseForDesc(
	std::string&& name,
	std::string&& source,
	std::string&& workingFolder,
	const ShaderEffectParserOptions& /*options*/,
	struct ShaderEffectDesc& effectDesc,
	ShaderEffectSource& effectSource
) noexcept {
	assert(!name.empty() && !source.empty() && !workingFolder.empty());

	effectDesc.name = std::move(name);
	effectSource.workingFolder = std::move(workingFolder);

	ParserState state = {
		.lineNumber = 1,
		.isNewLine = true
	};

	// 确保源代码以换行结尾，这可以有效简化文件结尾检查
	if (!source.ends_with('\n')) {
		source.push_back('\n');
	}
	// std::string_view sourceView(source);

	return false;
}

}

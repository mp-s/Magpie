#include "pch.h"
#include "EffectHelper.h"
#include "EffectInfo.h"
#include "LocalizationService.h"
#include "Logger.h"
#include "ShaderEffectDrawInfo.h"
#include "ShaderEffectParser.h"
#include "StrHelper.h"
#include <bitset>

namespace Magpie {

// 当前 MagpieFX 版本
static constexpr uint32_t MAGPIE_FX_VERSION = 5;
// 向后兼容的最低版本
static constexpr uint32_t MAGPIE_FX_MIN_SUPPORTED_VERSION = 4;

// 必须出现在一行的开头才视为指令
static const char* META_INDICATOR = "//!";

struct ParserState {
	std::string errorMsg;
	uint32_t lineNumber;
	bool isNewLine;
};

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

struct CommandInfo {
	const char* name;
	bool (*resolver)(std::string_view&, ParserState&, void*) noexcept;
	bool isRequired;
};

struct PassPropData {
	ShaderEffectPassDesc& passDesc;
	const SmallVectorImpl<ShaderEffectTextureDesc>& textures;
};

static std::string DebugFormat(std::string_view str) noexcept {
	return fmt::format("{:?s}", std::span<const char>(str.begin(), str.end()));
}

static void SetGeneralParseError(
	ParserState& state,
	std::string_view source,
	std::string_view expected = {}
) noexcept {
	// 限制打印的源代码字符数量
	std::string sourceStart;
	constexpr uint32_t SOURCE_PRINT_COUNT = 12;
	if (source.size() <= SOURCE_PRINT_COUNT) {
		sourceStart = DebugFormat(source);
	} else {
		sourceStart = StrHelper::Concat(source.substr(0, SOURCE_PRINT_COUNT), "...");
		sourceStart = DebugFormat(sourceStart);
	}

	if (expected.empty()) {
		std::string msgFmt = StrHelper::UTF16ToUTF8(LocalizationService::Get()
				.GetLocalizedString(L"ShaderEffectParser_GeneralError"));
		state.errorMsg = fmt::format(fmt::runtime(msgFmt), state.lineNumber, sourceStart);
	} else {
		std::string msgFmt = StrHelper::UTF16ToUTF8(LocalizationService::Get()
				.GetLocalizedString(L"ShaderEffectParser_GeneralErrorWithExpected"));
		state.errorMsg = fmt::format(fmt::runtime(msgFmt),
			state.lineNumber, sourceStart, DebugFormat(expected));
	}
}

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
					state.errorMsg = StrHelper::UTF16ToUTF8(LocalizationService::Get()
						.GetLocalizedString(L"ShaderEffectParser_UnclosedBlockComment"));
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
			SetGeneralParseError(state, source);
			return false;
		}
	}

	char c = source[0];

	// 必须以字母或下划线开头
	if (!StrHelper::isalpha(c) && c != '_') {
		SetGeneralParseError(state, source);
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
		SetGeneralParseError(state, token, expectedToken);
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

static bool RequireLineEnd(std::string_view& source, ParserState& state) noexcept {
	RemoveLeadingSpaces(source, state);

	if (source.empty() || source[0] == '\n') {
		return true;
	} else {
		SetGeneralParseError(state, source, "\n");
		return false;
	}
}

static bool RequireSourceEnd(std::string_view& source, ParserState& state) noexcept {
	RemoveLeadingBlanks(source, state);

	if (source.empty()) {
		return true;
	} else {
		SetGeneralParseError(state, source, "\n");
		return false;
	}
}

static bool CheckMagic(std::string_view& source, ParserState& state) noexcept {
	bool isMetaIndicator = false;
	if (!CheckMetaIndicator(source, state, isMetaIndicator)) {
		return false;
	}

	if (!isMetaIndicator) {
		SetGeneralParseError(state, source, META_INDICATOR);
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
			SetGeneralParseError(state, source);
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

	std::from_chars_result result = std::from_chars(source.data(), source.data() + source.size(), value);
	if (result.ec != std::errc{}) {
		SetGeneralParseError(state, source);
		return false;
	}

	source.remove_prefix(result.ptr - source.data());
	state.isNewLine = false;
	return true;
}

template <typename Fn>
static bool FindBlocks(
	std::string_view sourceView,
	const Fn& completeCurrentBlock,
	ParserState& state
) noexcept {
	for (size_t i = 0; i < sourceView.size(); ++i) {
		char c = sourceView[i];

		if (c == '\n') {
			++state.lineNumber;
			state.isNewLine = true;
		} else if (c == '/') {
			bool isComment;
			bool isMetaIdicator;
			if (!RemoveLeadingComment(sourceView, state, i, isComment, isMetaIdicator)) {
				return false;
			}

			if (isMetaIdicator) {
				std::string_view tempSource = sourceView.substr(i + 3);
				std::string_view token;
				if (!GetNextToken<false, false>(tempSource, state, token)) {
					return false;
				}

				std::string newBlockType = StrHelper::ToUpperCase(token);
				size_t newBlockOffset = tempSource.data() - sourceView.data();

				// sourceView[i - 1] 是换行符，因此每个区块都以换行结尾。区块开头不包含声明该区块的
				// 指令，如果该指令没有参数，此区块就以换行符开头。
				bool result = true;
				if (newBlockType == "PARAMETER") {
					result = completeCurrentBlock(BlockType::Parameter, i, newBlockOffset);
				} else if (newBlockType == "TEXTURE") {
					result = completeCurrentBlock(BlockType::Texture, i, newBlockOffset);
				} else if (newBlockType == "SAMPLER") {
					result = completeCurrentBlock(BlockType::Sampler, i, newBlockOffset);
				} else if (newBlockType == "COMMON") {
					result = completeCurrentBlock(BlockType::Common, i, newBlockOffset);
				} else if (newBlockType == "PASS") {
					result = completeCurrentBlock(BlockType::Pass, i, newBlockOffset);
				}
				if (!result) {
					return false;
				}

				// 下个循环会加一
				i = newBlockOffset - 1;
				state.isNewLine = false;
			}
		}
	}

	// 结束最后一个区块。sourceView 以换行符结尾，因此最后一个区块也以换行符结尾。
	return completeCurrentBlock(
		BlockType::Header, sourceView.size(), std::numeric_limits<size_t>::max());
}

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
			state.errorMsg = StrHelper::Concat("缺少 ", commandInfos[i].name);
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

	if (version < MAGPIE_FX_MIN_SUPPORTED_VERSION || version > MAGPIE_FX_VERSION) {
		state.errorMsg = StrHelper::UTF16ToUTF8(LocalizationService::Get()
			.GetLocalizedString(L"ShaderEffectParser_UnsupportedFXVersion"));
		return false;
	}

	return RequireLineEnd(source, state);
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

	((EffectInfo*)data)->sortName = sortName;
	return true;
}

static bool ResolveHeaderUse(
	std::string_view& source,
	ParserState& state,
	void* data
) noexcept {
	std::string_view flags;
	if (!GetNextStringUntilLineEnd<false>(source, state, flags)) {
		return false;
	}

	static constexpr std::array FLAG_INFOS = {
		std::make_pair("DYNAMIC", EffectInfoFlags::UseDynamic)
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

			((EffectInfo*)data)->flags |= it->second;
		} else {
			Logger::Get().Warn(StrHelper::Concat("未知的 USE 标志: ", token));
		}
	}

	return true;
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
		std::make_pair("FP16", EffectInfoFlags::SupportFP16),
		std::make_pair("ADVANCEDCOLOR", EffectInfoFlags::SupportAdvancedColor)
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

			((EffectInfo*)data)->flags |= it->second;
		} else {
			Logger::Get().Warn(StrHelper::Concat("未知的 CAPABILITY 标志: ", token));
		}
	}

	return true;
}

static bool ResolveHeaderScaleFactor(
	std::string_view& source,
	ParserState& state,
	void* data
) noexcept {
	if (!GetNextNumber(source, state, ((EffectInfo*)data)->scaleFactor)) {
		return false;
	}

	return RequireLineEnd(source, state);
}

static bool ResolveHeaderBlock(
	std::string_view source,
	ParserState& state,
	EffectInfo& effectInfo
) noexcept {
	static constexpr std::array COMMAND_INFOS = {
		CommandInfo{ "VERSION", ResolveHeaderVersion, true },
		CommandInfo{ "SORT_NAME", ResolveHeaderSortName, false },
		CommandInfo{ "USE", ResolveHeaderUse, false },
		CommandInfo{ "CAPABILITY", ResolveHeaderCapability, false },
		CommandInfo{ "SCALE_FACTOR", ResolveHeaderScaleFactor, false },
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
		SetGeneralParseError(state, source, "\n");
		return false;
	}

	// 跳过整行
	std::string_view unused;
	if (!GetNextStringUntilLineEnd<false>(source, state, unused)) {
		return false;
	}

	return RequireSourceEnd(source, state);
}

static bool ResolveParameterDefault(
	std::string_view& source,
	ParserState& state,
	void* data
) noexcept {
	if (!GetNextNumber(source, state, ((EffectInfoParameter*)data)->defaultValue)) {
		return false;
	}

	return RequireLineEnd(source, state);
}

static bool ResolveParameterMin(
	std::string_view& source,
	ParserState& state,
	void* data
) noexcept {
	if (!GetNextNumber(source, state, ((EffectInfoParameter*)data)->minValue)) {
		return false;
	}

	return RequireLineEnd(source, state);
}

static bool ResolveParameterMax(
	std::string_view& source,
	ParserState& state,
	void* data
) noexcept {
	if (!GetNextNumber(source, state, ((EffectInfoParameter*)data)->maxValue)) {
		return false;
	}

	return RequireLineEnd(source, state);
}

static bool ResolveParameterStep(
	std::string_view& source,
	ParserState& state,
	void* data
) noexcept {
	if (!GetNextNumber(source, state, ((EffectInfoParameter*)data)->step)) {
		return false;
	}

	return RequireLineEnd(source, state);
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

static bool ResolveParameterBlock(
	std::string_view source,
	ParserState& state,
	EffectInfoParameter& effectInfoParameter
) noexcept {
	static constexpr std::array COMMAND_INFOS = {
		CommandInfo{ "DEFAULT", ResolveParameterDefault, true },
		CommandInfo{ "MIN", ResolveParameterMin, true },
		CommandInfo{ "MAX", ResolveParameterMax, true },
		CommandInfo{ "STEP", ResolveParameterStep, true },
		CommandInfo{ "LABEL", ResolveParameterLabel, false }
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
		SetGeneralParseError(state, token, "float|int");
		return false;
	}

	if (!GetNextToken<true, false>(source, state, token)) {
		return false;
	}

	effectInfoParameter.name = token;

	if (!RemoveLeadingBlanks(source, state)) {
		return false;
	}

	if (!source.starts_with(';')) {
		SetGeneralParseError(state, source, ";");
		return false;
	}

	source.remove_prefix(1);
	state.isNewLine = false;

	return RequireSourceEnd(source, state);
}

static bool ResolveTextureWidth(
	std::string_view& source,
	ParserState& state,
	void* data
) noexcept {
	std::string_view expr;
	if (!GetNextStringUntilLineEnd<false>(source, state, expr)) {
		return false;
	}

	((ShaderEffectTextureDesc*)data)->widthExpr = expr;
	return true;
}

static bool ResolveTextureHeight(
	std::string_view& source,
	ParserState& state,
	void* data
) noexcept {
	std::string_view expr;
	if (!GetNextStringUntilLineEnd<false>(source, state, expr)) {
		return false;
	}

	((ShaderEffectTextureDesc*)data)->heightExpr = expr;
	return true;
}

static bool ResolveTextureFormat(
	std::string_view& source,
	ParserState& state,
	void* data
) noexcept {
	std::string_view token;
	if (!GetNextToken<false, false>(source, state, token)) {
		return false;
	}

	static const auto formatMap = [] {
		phmap::flat_hash_map<std::string, ShaderEffectTextureFormat> result;

		// UNKNOWN 不可用
		constexpr size_t descCount = std::size(EffectHelper::SHADER_TEXTURE_FORMAT_DESCS) - 1;
		result.reserve(descCount);
		for (size_t i = 1; i < descCount; ++i) {
			result.emplace(
				EffectHelper::SHADER_TEXTURE_FORMAT_DESCS[i].name,
				(ShaderEffectTextureFormat)i
			);
		}
		return result;
	}();

	auto it = formatMap.find(StrHelper::ToUpperCase(token));
	if (it == formatMap.end()) {
		SetGeneralParseError(state, token);
		return false;
	}
	((ShaderEffectTextureDesc*)data)->format = it->second;

	return RequireLineEnd(source, state);
}

static bool ResolveTextureSource(
	std::string_view& source,
	ParserState& state,
	void* data
) noexcept {
	std::string_view value;
	if (!GetNextStringUntilLineEnd<false>(source, state, value)) {
		return false;
	}

	((ShaderEffectTextureDesc*)data)->source = value;
	return true;
}

static bool ResolveTextureBlock(
	std::string_view source,
	ParserState& state, 
	ShaderEffectTextureDesc& desc
) noexcept {
	static constexpr std::array COMMAND_INFOS = {
		CommandInfo{ "WIDTH", ResolveTextureWidth, false },
		CommandInfo{ "HEIGHT", ResolveTextureHeight, false },
		CommandInfo{ "FORMAT", ResolveTextureFormat, false },
		CommandInfo{ "SOURCE", ResolveTextureSource, false }
	};

	if (!ResolveBlockCommon(COMMAND_INFOS, source, state, &desc)) {
		return false;
	}

	if (!CheckNextToken<true>(source, state, "Texture2D")) {
		return false;
	}

	std::string_view token;
	if (!GetNextToken<true, false>(source, state, token)) {
		return false;
	}

	if (token == "INPUT" || token == "OUTPUT") {
		// INPUT 和 OUTPUT 不允许有属性
		if (!desc.widthExpr.empty() || !desc.heightExpr.empty() ||
			desc.format != ShaderEffectTextureFormat::UNKNOWN || !desc.source.empty())
		{
			state.errorMsg = "INPUT 和 OUTPUT 不允许有属性";
			return false;
		}
	} else {
		desc.name = token;

		if (desc.source.empty()) {
			// 不存在 SOURCE 属性时必须指定 WIDTH、HEIGHT 和 FORMAT
			if (desc.widthExpr.empty()) {
				state.errorMsg = "缺少 WIDTH 属性";
				return false;
			}

			if (desc.heightExpr.empty()) {
				state.errorMsg = "缺少 HEIGHT 属性";
				return false;
			}

			if (desc.format == ShaderEffectTextureFormat::UNKNOWN) {
				state.errorMsg = "缺少 FORMAT 属性";
				return false;
			}
		} else {
			// SOURCE 和 WIDTH/HEIGHT 冲突
			if (!desc.widthExpr.empty() || !desc.heightExpr.empty()) {
				state.errorMsg = "SOURCE 和 WIDTH/HEIGHT 冲突";
				return false;
			}

			// 存在 SOURCE 时 FORMAT 可选，默认值为 R16G16B16A16_FLOAT
			if (desc.format == ShaderEffectTextureFormat::UNKNOWN) {
				desc.format = ShaderEffectTextureFormat::R16G16B16A16_FLOAT;
			}
		}
	}

	if (!RemoveLeadingBlanks(source, state)) {
		return false;
	}

	if (!source.starts_with(';')) {
		SetGeneralParseError(state, source, ";");
		return false;
	}

	source.remove_prefix(1);
	state.isNewLine = false;

	return RequireSourceEnd(source, state);
}

static bool ResolveSamplerFilter(
	std::string_view& source,
	ParserState& state,
	void* data
) noexcept {
	std::string_view token;
	if (!GetNextToken<false, false>(source, state, token)) {
		return false;
	}

	std::string tokenUpper = StrHelper::ToUpperCase(token);
	if (tokenUpper == "LINEAR") {
		((ShaderEffectSamplerDesc*)data)->filterType = ShaderEffectSamplerFilterType::Linear;
	} else if (tokenUpper == "POINT") {
		((ShaderEffectSamplerDesc*)data)->filterType = ShaderEffectSamplerFilterType::Point;
	} else {
		SetGeneralParseError(state, token, "LINEAR|POINT");
	}

	return RequireLineEnd(source, state);
}

static bool ResolveSamplerAddress(
	std::string_view& source,
	ParserState& state,
	void* data
) noexcept {
	std::string_view token;
	if (!GetNextToken<false, false>(source, state, token)) {
		return false;
	}

	std::string tokenUpper = StrHelper::ToUpperCase(token);
	if (tokenUpper == "CLAMP") {
		((ShaderEffectSamplerDesc*)data)->addressType = ShaderEffectSamplerAddressType::Clamp;
	} else if (tokenUpper == "WRAP") {
		((ShaderEffectSamplerDesc*)data)->addressType = ShaderEffectSamplerAddressType::Wrap;
	} else {
		SetGeneralParseError(state, token, "CLAMP|WRAP");
	}

	return RequireLineEnd(source, state);
}

static bool ResolveSamplerBlock(
	std::string_view source,
	ParserState& state,
	ShaderEffectSamplerDesc& desc
) noexcept {
	static constexpr std::array COMMAND_INFOS = {
		CommandInfo{ "FILTER", ResolveSamplerFilter, true },
		CommandInfo{ "ADDRESS", ResolveSamplerAddress, false },
	};

	if (!ResolveBlockCommon(COMMAND_INFOS, source, state, &desc)) {
		return false;
	}

	if (!CheckNextToken<true>(source, state, "SamplerState")) {
		return false;
	}

	std::string_view token;
	if (!GetNextToken<true, false>(source, state, token)) {
		return false;
	}

	desc.name = token;

	if (!RemoveLeadingBlanks(source, state)) {
		return false;
	}

	if (!source.starts_with(';')) {
		SetGeneralParseError(state, source, ";");
		return false;
	}

	source.remove_prefix(1);
	state.isNewLine = false;

	return RequireSourceEnd(source, state);
}

static bool ResolvePassOut(
	std::string_view& source,
	ParserState& state,
	void* data
) noexcept {
	auto& [desc, textures] = *(PassPropData*)data;

	std::string_view value;
	if (!GetNextStringUntilLineEnd<false>(source, state, value)) {
		return false;
	}

	for (std::string_view& token : StrHelper::Split(value, ',')) {
		StrHelper::Trim(token);

		// 0: INPUT (不允许)
		// 1: OUTPUT
		// 2+: 中间纹理
		if (token == "INPUT") {
			state.errorMsg = "INPUT 不能作为通道输出";
			return false;
		} else if (token == "OUTPUT") {
			desc.outputs.push_back(1);
		} else {
			auto it = std::find_if(textures.begin(), textures.end(), [&](const auto& textureDesc) {
				return textureDesc.name == token;
			});
			if (it == textures.end()) {
				SetGeneralParseError(state, token);
				return false;
			}

			if (it->source.empty()) {
				desc.outputs.push_back(uint32_t(it - textures.begin()) + 2);
			} else {
				state.errorMsg = fmt::format("只读纹理 {} 不能作为通道输出", it->name);
				return false;
			}
		}
	}

	std::sort(desc.outputs.begin(), desc.outputs.end());
	
	// 检查重复成员
	for (size_t i = 0, end = desc.outputs.size() - 1; i < end; ++i) {
		if (desc.outputs[i] == desc.outputs[i + 1]) {
			state.errorMsg = "OUT 存在重复成员";
			return false;
		}
	}

	return true;
}

static bool ResolvePassIn(
	std::string_view& source,
	ParserState& state,
	void* data
) noexcept {
	auto& [desc, textures] = *(PassPropData*)data;

	std::string_view value;
	if (!GetNextStringUntilLineEnd<false>(source, state, value)) {
		return false;
	}

	for (std::string_view& token : StrHelper::Split(value, ',')) {
		StrHelper::Trim(token);

		// 0: INPUT
		// 1: OUTPUT (不允许)
		// 2+: 中间纹理
		if (token == "INPUT") {
			desc.inputs.push_back(0);
		} else if (token == "OUTPUT") {
			state.errorMsg = "OUTPUT 不能作为通道输入";
			return false;
		} else {
			auto it = std::find_if(textures.begin(), textures.end(), [&](const auto& textureDesc) {
				return textureDesc.name == token;
			});
			if (it == textures.end()) {
				SetGeneralParseError(state, token);
				return false;
			}

			desc.inputs.push_back(uint32_t(it - textures.begin()) + 2);
		}
	}

	std::sort(desc.inputs.begin(), desc.inputs.end());

	// 检查重复成员
	for (size_t i = 0, end = desc.inputs.size() - 1; i < end; ++i) {
		if (desc.inputs[i] == desc.inputs[i + 1]) {
			state.errorMsg = "IN 存在重复成员";
			return false;
		}
	}

	return true;
}

static bool ResolvePassDesc(
	std::string_view& source,
	ParserState& state,
	void* data
) noexcept {
	std::string_view value;
	if (!GetNextStringUntilLineEnd<false>(source, state, value)) {
		return false;
	}

	((PassPropData*)data)->passDesc.desc = value;
	return true;
}

static bool ResolvePassStyle(
	std::string_view& source,
	ParserState& state,
	void* data
) noexcept {
	std::string_view token;
	if (!GetNextToken<false, false>(source, state, token)) {
		return false;
	}

	std::string tokenUpper = StrHelper::ToUpperCase(token);
	if (tokenUpper == "PS") {
		((PassPropData*)data)->passDesc.flags |= ShaderEffectPassFlags::PSStyle;
	} else if (tokenUpper != "CS") {
		SetGeneralParseError(state, token, "CS|PS");
	}

	return RequireLineEnd(source, state);
}

static bool ResolvePassBlockSize(
	std::string_view& source,
	ParserState& state,
	void* data
) noexcept {
	SizeU& blockSize = ((PassPropData*)data)->passDesc.blockSize;

	std::string_view value;
	if (!GetNextStringUntilLineEnd<false>(source, state, value)) {
		return false;
	}

	SmallVector<std::string_view> split = StrHelper::Split(value, ',');
	if (split.size() > 2) {
		state.errorMsg = "BLOCK_SIZE 最多允许两个成员";
		return false;
	}

	const char* last = split[0].data() + split[0].size();
	std::from_chars_result result = std::from_chars(split[0].data(), last, blockSize.width);
	if (result.ec != std::errc{} || result.ptr != last) {
		SetGeneralParseError(state, split[0]);
		return false;
	}

	if (split.size() == 2) {
		last = split[1].data() + split[1].size();
		result = std::from_chars(split[1].data(), last, blockSize.height);
		if (result.ec != std::errc{} || result.ptr != last) {
			SetGeneralParseError(state, split[1]);
			return false;
		}
	} else {
		// 只有一个成员则同时指定长和高
		blockSize.height = blockSize.width;
	}
	
	return true;
}

static bool ResolvePassNumThreads(
	std::string_view& source,
	ParserState& state,
	void* data
) noexcept {
	std::array<uint32_t, 3>& numThreads = ((PassPropData*)data)->passDesc.numThreads;

	std::string_view value;
	if (!GetNextStringUntilLineEnd<false>(source, state, value)) {
		return false;
	}

	SmallVector<std::string_view> split = StrHelper::Split(value, ',');
	uint32_t elemCount = (uint32_t)split.size();
	if (elemCount > 3) {
		state.errorMsg = "NUM_THREADS 最多允许三个成员";
		return false;
	}

	for (uint32_t i = 0; i < 3; ++i) {
		if (i < elemCount) {
			const char* last = split[i].data() + split[i].size();
			std::from_chars_result result = std::from_chars(split[i].data(), last, numThreads[i]);
			if (result.ec != std::errc{} || result.ptr != last) {
				SetGeneralParseError(state, split[i]);
				return false;
			}
		} else {
			// 未指定则置一
			numThreads[i] = 1;
		}
	}
	
	return true;
}

static bool ResolvePassBlock(
	std::string_view& source,
	ParserState& state,
	ShaderEffectDrawInfo& drawInfo
) noexcept {
	size_t passIdx;
	if (!GetNextNumber(source, state, passIdx)) {
		return false;
	}

	if (!RequireLineEnd(source, state)) {
		return false;
	}

	// Pass 序号从 1 开始
	if (passIdx == 0 || passIdx > drawInfo.passes.size()) {
		state.errorMsg = "通道序号错误";
		return false;
	}

	ShaderEffectPassDesc& curPassDesc = drawInfo.passes[passIdx - 1];

	if (!curPassDesc.outputs.empty()) {
		state.errorMsg = fmt::format("存在多个 Pass{}", passIdx);
		return false;
	}

	static constexpr std::array COMMAND_INFOS = {
		CommandInfo{ "OUT", ResolvePassOut, true },
		CommandInfo{ "IN", ResolvePassIn, false },
		CommandInfo{ "DESC", ResolvePassDesc, false },
		CommandInfo{ "STYLE", ResolvePassStyle, false },
		CommandInfo{ "BLOCK_SIZE", ResolvePassBlockSize, false },
		CommandInfo{ "NUM_THREADS", ResolvePassNumThreads, false }
	};

	PassPropData data = { curPassDesc, drawInfo.textures };
	if (!ResolveBlockCommon(COMMAND_INFOS, source, state, &data)) {
		return false;
	}

	if (bool(curPassDesc.flags & ShaderEffectPassFlags::PSStyle)) {
		if (curPassDesc.blockSize.width != 0 || curPassDesc.numThreads[0] != 0) {
			state.errorMsg = "PS 风格不得出现 BLOCK_SIZE 和 NUM_THREADS";
			return false;
		}

		curPassDesc.blockSize = { 16,16 };
		curPassDesc.numThreads = { 64,1,1 };
	} else {
		if (curPassDesc.blockSize.width == 0 || curPassDesc.numThreads[0] == 0) {
			state.errorMsg = "CS 风格必须指定 BLOCK_SIZE 和 NUM_THREADS";
			return false;
		}
	}

	// 不允许同一纹理同时作为输入和输出
	if (auto it = std::set_intersection(
		curPassDesc.inputs.begin(),
		curPassDesc.inputs.end(),
		curPassDesc.outputs.begin(),
		curPassDesc.outputs.end(),
		curPassDesc.inputs.begin()
	); it != curPassDesc.inputs.begin()) {
		uint32_t texIdx = curPassDesc.inputs[0];
		// INPUT 和 OUTPUT 不可能是交集
		assert(texIdx >= 2);
		const std::string& texName = drawInfo.textures[size_t(texIdx - 2)].name;
		state.errorMsg = fmt::format("纹理 {0} 同时作为 Pass{1} 的输入和输出", texName, passIdx);
		return false;
	}

	// 最后一个通道的输出只能是 OUTPUT
	if (passIdx == drawInfo.passes.size()) {
		if (curPassDesc.outputs.size() != 1 || curPassDesc.outputs[0] != 1) {
			state.errorMsg = "最后一个通道的输出只能是 OUTPUT";
			return false;
		}
	}

	// 生成默认描述
	if (curPassDesc.desc.empty()) {
		curPassDesc.desc = fmt::format("Pass {}", passIdx);
	}

	return true;
}

std::string ShaderEffectParser::ParseForInfo(
	std::string&& name,
	std::string&& source,
	EffectInfo& effectInfo
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
		Logger::Get().Error(StrHelper::Concat("CheckMagic 失败\n\t错误消息: ", state.errorMsg));
		return std::move(state.errorMsg);
	}

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
		return true;
	};

	if (!FindBlocks(sourceView, completeCurrentBlock, state)) {
		Logger::Get().Error(StrHelper::Concat("FindBlocks 失败\n\t错误消息: ", state.errorMsg));
		return std::move(state.errorMsg);
	}

	state.lineNumber = headerBlock.startLineNumer;
	state.isNewLine = false;
	if (!ResolveHeaderBlock(headerBlock.source, state, effectInfo)) {
		Logger::Get().Error(StrHelper::Concat("ResolveHeaderBlock 失败\n\t错误消息: ", state.errorMsg));
		return std::move(state.errorMsg);
	}

	effectInfo.params.resize(paramBlocks.size());
	for (size_t i = 0; i < paramBlocks.size(); ++i) {
		state.lineNumber = paramBlocks[i].startLineNumer;
		state.isNewLine = false;
		if (!ResolveParameterBlock(paramBlocks[i].source, state, effectInfo.params[i])) {
			Logger::Get().Error(fmt::format(
				"ResolveParameterBlock#{} 失败\n\t错误消息: {}", i, state.errorMsg));
			return std::move(state.errorMsg);
		}
	}

	// 检查是否有重复的标识符
	{
		phmap::flat_hash_set<std::string_view> names;
		for (const EffectInfoParameter& param : effectInfo.params) {
			if (names.contains(param.name)) {
				state.errorMsg = fmt::format("标识符 \"{}\" 重复", param.name);
				return std::move(state.errorMsg);
			} else {
				names.insert(param.name);
			}
		}
	}
	
	return std::move(state.errorMsg);
}

static bool CheckForDuplicateName(
	const EffectInfo& effectInfo,
	const ShaderEffectDrawInfo& drawInfo,
	ParserState& state
) noexcept {
	phmap::flat_hash_set<std::string_view> names;
	std::string_view dupName;

	// 参数已在 ParseForInfo 中检查过
	for (const EffectInfoParameter& param : effectInfo.params) {
		names.emplace(param.name);
	}

	for (const ShaderEffectTextureDesc& textureDesc : drawInfo.textures) {
		if (names.contains(textureDesc.name)) {
			dupName = textureDesc.name;
			break;
		} else {
			names.emplace(textureDesc.name);
		}
	}

	if (dupName.empty()) {
		for (const ShaderEffectSamplerDesc& samplerDesc : drawInfo.samplers) {
			if (names.contains(samplerDesc.name)) {
				dupName = samplerDesc.name;
				break;
			} else {
				names.emplace(samplerDesc.name);
			}
		}
	}
	
	if (dupName.empty()) {
		return true;
	} else {
		state.errorMsg = fmt::format("标识符 \"{}\" 重复", dupName);
		return false;
	}
}

std::string ShaderEffectParser::ParseForDesc(
	const EffectInfo& effectInfo,
	std::string&& source,
	const ShaderEffectParserOptions& /*options*/,
	ShaderEffectDrawInfo& drawInfo,
	ShaderEffectSource& /*effectSource*/
) noexcept {
	assert(!source.empty());

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
		Logger::Get().Error(StrHelper::Concat("CheckMagic 失败\n\t错误消息: ", state.errorMsg));
		return std::move(state.errorMsg);
	}

	SmallVector<BlockData> textureBlocks;
	SmallVector<BlockData> samplerBlocks;
	BlockData commonBlock{};
	SmallVector<BlockData> passBlocks;

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
		case BlockType::Parameter:
			break;
		case BlockType::Texture:
			textureBlocks.push_back(BlockData{ sourceView.substr(
				curBlockOffset, curBlockEnd - curBlockOffset),curBlockStartLineNumber });
			break;
		case BlockType::Sampler:
			samplerBlocks.push_back(BlockData{ sourceView.substr(
				curBlockOffset, curBlockEnd - curBlockOffset),curBlockStartLineNumber });
			break;
		case BlockType::Common:
			if (commonBlock.source.empty()) {
				commonBlock.source =
					sourceView.substr(curBlockOffset, curBlockEnd - curBlockOffset);
				commonBlock.startLineNumer = curBlockStartLineNumber;
				break;
			} else {
				state.errorMsg = "只允许存在一个 COMMON 块";
				return false;
			}
		case BlockType::Pass:
			passBlocks.push_back(BlockData{ sourceView.substr(
				curBlockOffset, curBlockEnd - curBlockOffset),curBlockStartLineNumber });
			break;
		default:
			assert(false);
			break;
		}

		curBlockType = newBlockType;
		curBlockOffset = newBlockOffset;
		curBlockStartLineNumber = state.lineNumber;
		return true;
	};

	if (!FindBlocks(sourceView, completeCurrentBlock, state)) {
		Logger::Get().Error(StrHelper::Concat("FindBlocks 失败\n\t错误消息: ", state.errorMsg));
		return std::move(state.errorMsg);
	}

	if (passBlocks.empty()) {
		state.errorMsg = "未找到 PASS 块";
		return std::move(state.errorMsg);
	}

	for (size_t i = 0; i < textureBlocks.size(); ++i) {
		state.lineNumber = textureBlocks[i].startLineNumer;
		state.isNewLine = false;

		ShaderEffectTextureDesc desc;
		if (!ResolveTextureBlock(textureBlocks[i].source, state, desc)) {
			Logger::Get().Error(fmt::format(
				"ResolveTextureBlock#{} 失败\n\t错误消息: {}", i, state.errorMsg));
			return std::move(state.errorMsg);
		}

		// 排除 INPUT 和 OUTPUT
		if (!desc.name.empty()) {
			drawInfo.textures.push_back(std::move(desc));
		}
	}

	drawInfo.samplers.resize(samplerBlocks.size());
	for (size_t i = 0; i < samplerBlocks.size(); ++i) {
		state.lineNumber = samplerBlocks[i].startLineNumer;
		state.isNewLine = false;

		if (!ResolveSamplerBlock(samplerBlocks[i].source, state, drawInfo.samplers[i])) {
			Logger::Get().Error(fmt::format(
				"ResolveSamplerBlock#{} 失败\n\t错误消息: {}", i, state.errorMsg));
			return std::move(state.errorMsg);
		}
	}

	if (!CheckForDuplicateName(effectInfo, drawInfo, state)) {
		Logger::Get().Error(StrHelper::Concat(
			"CheckForDuplicateName 失败\n\t错误消息: ", state.errorMsg));
		return std::move(state.errorMsg);
	}

	if (!commonBlock.source.empty()) {
		state.lineNumber = commonBlock.startLineNumer;
		state.isNewLine = false;
		if (!RemoveLeadingBlanks(commonBlock.source, state)) {
			return std::move(state.errorMsg);
		}
		commonBlock.startLineNumer = state.lineNumber;
	}
	
	drawInfo.passes.resize(passBlocks.size());
	for (size_t i = 0; i < passBlocks.size(); ++i) {
		BlockData& curBlock = passBlocks[i];

		state.lineNumber = curBlock.startLineNumer;
		state.isNewLine = false;

		// 这会修改 curBlock.source
		if (!ResolvePassBlock(curBlock.source, state, drawInfo)) {
			Logger::Get().Error(fmt::format(
				"ResolvePassBlock#{} 失败\n\t错误消息: {}", i, state.errorMsg));
			return std::move(state.errorMsg);
		}
		curBlock.startLineNumer = state.lineNumber;
	}
	
	return std::move(state.errorMsg);
}

}

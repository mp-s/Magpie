#pragma once
#include <parallel_hashmap/phmap.h>

namespace Magpie {

struct EffectInfo;
struct ShaderEffectDrawInfo;

enum class ShaderEffectParserFlags {
	None = 0,
	EnableFP16 = 1,
	EnableAdvancedColor = 1 << 1
};
DEFINE_ENUM_FLAG_OPERATORS(ShaderEffectParserFlags)

struct ShaderEffectParserOptions {
	// 为空表示不内联
	const phmap::flat_hash_map<std::string, float>* inlineParams = nullptr;
	D3D_SHADER_MODEL shaderModel = D3D_SHADER_MODEL_5_1;
	ShaderEffectParserFlags flags = ShaderEffectParserFlags::None;
};

struct ShaderEffectSource {
	std::vector<std::string> sources;
	std::vector<std::pair<std::string, std::string>> macros;
	D3D_SHADER_MODEL shaderModel = D3D_SHADER_MODEL_5_1;
};

struct ShaderEffectParser {
	// 成功时返回空字符串，否则返回错误消息
	static std::string ParseForInfo(
		std::string&& name,
		std::string&& source,
		EffectInfo& effectInfo
	) noexcept;

	static std::string ParseForDesc(
		const EffectInfo& effectInfo,
		std::string&& source,
		const ShaderEffectParserOptions& options,
		ShaderEffectDrawInfo& drawInfo,
		ShaderEffectSource& effectSource
	) noexcept;
};

}

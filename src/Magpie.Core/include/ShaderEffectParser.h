#pragma once
#include <parallel_hashmap/phmap.h>

namespace Magpie {

enum class ShaderEffectParserFlags {
	None = 0,
	// 只在效果支持 FP16 时影响字节码
	EnableFP16 = 1,
	// 只在效果同时支持 linear sRGB 和 scRGB 时影响字节码
	EnableAdvancedColor = 1 << 1
};
DEFINE_ENUM_FLAG_OPERATORS(ShaderEffectParserFlags)

struct ShaderEffectParserOptions {
	// 只在效果存在参数时影响字节码。为空表示不内联
	const phmap::flat_hash_map<std::string, float>* inlineParams = nullptr;
	// 只在效果支持 shader model 时影响字节码
	D3D_SHADER_MODEL shaderModel = D3D_SHADER_MODEL_5_1;
	ShaderEffectParserFlags flags = ShaderEffectParserFlags::None;
};

struct ShaderEffectSource {
	std::vector<std::string> sources;
	// 用于解析 #include
	std::string workingFolder;
	std::vector<std::pair<std::string, std::string>> macros;
	D3D_SHADER_MODEL shaderModel = D3D_SHADER_MODEL_5_1;
};

struct ShaderEffectParser {
	static bool ParseForInfo(std::string&& name, std::string&& source, struct EffectInfo& effectInfo) noexcept;

	static bool ParseForDesc(
		std::string&& name,
		std::string&& source,
		std::string&& workingFolder,
		const ShaderEffectParserOptions& options,
		struct ShaderEffectDesc& effectDesc,
		ShaderEffectSource& effectSource
	) noexcept;
};

}

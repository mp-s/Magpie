#pragma once

namespace Magpie {

struct EffectInfoParameter {
	std::string name;
	std::string label;

	float defaultValue;
	float minValue;
	float maxValue;
	float step;
};

enum class EffectInfoFlags {
	None,
	SupportFP16 = 1,
	// 必须支持一个或 linear sRGB + scRGB
	SupportLinearSRGB = 1 << 1,
	SupportScRGB = 1 << 2,
	SupportSRGB = 1 << 3
};
DEFINE_ENUM_FLAG_OPERATORS(EffectInfoFlags)

struct EffectInfo {
	std::string name;
	std::string sortName;
	std::vector<EffectInfoParameter> params;
	// 可使用 INPUT_WIDTH，空字符串表示支持自由缩放
	std::pair<std::string, std::string> outputSizeExprs;
	EffectInfoFlags flags = EffectInfoFlags::SupportLinearSRGB;
};

}

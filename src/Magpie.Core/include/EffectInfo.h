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

enum class EffectInfoFlags2 {
	None,
	SupportFP16 = 1,
	SupportAdvancedColor = 1 << 1,
};
DEFINE_ENUM_FLAG_OPERATORS(EffectInfoFlags2)

struct EffectInfo2 {
	std::string name;
	std::string sortName;
	std::vector<EffectInfoParameter> params;
	// 可使用 INPUT_WIDTH，空字符串表示支持自由缩放
	std::pair<std::string, std::string> outputSizeExprs;
	EffectInfoFlags2 flags = EffectInfoFlags2::None;
};

}

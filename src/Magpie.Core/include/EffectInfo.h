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
	// 0 表示可以自由缩放
	uint32_t scaleFactor = 0;
	EffectInfoFlags2 flags = EffectInfoFlags2::None;
};

}

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
	SupportAdvancedColor = 1 << 1,
	UseDynamic = 1 << 2
};
DEFINE_ENUM_FLAG_OPERATORS(EffectInfoFlags)

struct EffectInfo {
	std::string name;
	std::string sortName;
	std::vector<EffectInfoParameter> params;
	// 0 表示可以自由缩放
	uint32_t scaleFactor = 0;
	// 0 表示不限制
	uint32_t minFrameRate = 0;
	EffectInfoFlags flags = EffectInfoFlags::None;
};

}

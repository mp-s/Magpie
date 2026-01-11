#pragma once

namespace Magpie {

enum class EffectColorSpace {
	linear_sRGB,
	sRGB,
	scRGB
};

class EffectDrawerBase {
public:
	EffectDrawerBase() = default;
	EffectDrawerBase(const EffectDrawerBase&) = delete;
	EffectDrawerBase(EffectDrawerBase&&) = delete;

	virtual ~EffectDrawerBase() noexcept = default;
};

}

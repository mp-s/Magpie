#pragma once

namespace Magpie {

class EffectDrawerBase {
public:
	EffectDrawerBase() = default;
	EffectDrawerBase(const EffectDrawerBase&) = delete;
	EffectDrawerBase(EffectDrawerBase&&) = delete;

	virtual ~EffectDrawerBase() noexcept = default;
};

}

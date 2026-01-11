#pragma once
#include "EffectDrawerBase.h"
#include "EffectDesc.h"

namespace Magpie {

class ShaderEffectDrawer : public EffectDrawerBase {
public:
	virtual ~ShaderEffectDrawer() noexcept = default;

	static uint32_t CalcDescriptorCount( bool isFirst, bool isLast);


};

}

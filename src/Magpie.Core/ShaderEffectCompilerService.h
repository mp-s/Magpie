#pragma once
#include "ShaderEffectDrawInfo.h"

namespace Magpie {

class ShaderEffectCompilerService {
public:
	static ShaderEffectCompilerService& Get() noexcept {
		static ShaderEffectCompilerService instance;
		return instance;
	}

	ShaderEffectCompilerService(const ShaderEffectCompilerService&) = delete;
	ShaderEffectCompilerService(ShaderEffectCompilerService&&) = delete;

	bool Submit(std::string_view name, ShaderEffectDrawInfo& desc) const noexcept;

private:
	ShaderEffectCompilerService() = default;
};

}

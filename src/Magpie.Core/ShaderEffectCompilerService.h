#pragma once
#include "ShaderEffectDesc.h"

namespace Magpie {

class ShaderEffectCompilerService {
public:
	static ShaderEffectCompilerService& Get() noexcept {
		static ShaderEffectCompilerService instance;
		return instance;
	}

	ShaderEffectCompilerService(const ShaderEffectCompilerService&) = delete;
	ShaderEffectCompilerService(ShaderEffectCompilerService&&) = delete;

	bool Submit(std::string_view name, ShaderEffectDesc& desc) const noexcept;

private:
	ShaderEffectCompilerService() = default;
};

}

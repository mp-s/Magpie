#pragma once
#include "SmallVector.h"

namespace Magpie {

enum class ShaderEffectTextureFormat {
	UNKNOWN,
	R8_UNORM,
	R16_UNORM,
	R16_FLOAT,
	R8G8_UNORM,
	R32_FLOAT,
	R16G16_UNORM,
	R16G16_FLOAT,
	R11G11B10_FLOAT,
	R8G8B8A8_UNORM,
	R10G10B10A2_UNORM,
	R32G32_FLOAT,
	R16G16B16A16_UNORM,
	R16G16B16A16_FLOAT,
	R32G32B32A32_FLOAT
};

struct ShaderEffectTextureDesc {
	ShaderEffectTextureFormat format = ShaderEffectTextureFormat::UNKNOWN;
	std::pair<std::string, std::string> sizeExpr;
	std::string source;
};

enum class EffectSamplerFilterType {
	Point,
	Linear
};

enum class EffectSamplerAddressType {
	Clamp,
	Wrap
};

struct ShaderEffectSamplerDesc {
	EffectSamplerFilterType filterType = EffectSamplerFilterType::Point;
	EffectSamplerAddressType addressType = EffectSamplerAddressType::Clamp;
};

enum class ShaderEffectPassFlags {
	None = 0,
	PSStyle = 1
};
DEFINE_ENUM_FLAG_OPERATORS(ShaderEffectPassFlags)

struct ShaderEffectPassDesc {
	winrt::com_ptr<ID3DBlob> byteCode;
	// 0: INPUT
	// 1: OUTPUT
	// 2+: 中间纹理
	SmallVector<uint32_t> inputs;
	SmallVector<uint32_t> outputs;
	std::array<uint32_t, 3> numThreads{};
	SizeU blockSize{};
	ShaderEffectPassFlags flags = ShaderEffectPassFlags::None;

	// 用于在叠加层中显示
	std::string desc;
};

struct ShaderEffectDesc {
	// 不包含 INPUT 和 OUTPUT
	SmallVector<ShaderEffectTextureDesc, 0> textures;
	SmallVector<ShaderEffectSamplerDesc> samplers;
	SmallVector<ShaderEffectPassDesc, 0> passes;
};

}

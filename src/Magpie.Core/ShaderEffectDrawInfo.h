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
	std::string name;
	ShaderEffectTextureFormat format = ShaderEffectTextureFormat::UNKNOWN;
	std::string widthExpr;
	std::string heightExpr;
	std::string source;
};

enum class ShaderEffectSamplerFilterType {
	Point,
	Linear
};

enum class ShaderEffectSamplerAddressType {
	Clamp,
	Wrap
};

struct ShaderEffectSamplerDesc {
	std::string name;
	ShaderEffectSamplerFilterType filterType = ShaderEffectSamplerFilterType::Point;
	ShaderEffectSamplerAddressType addressType = ShaderEffectSamplerAddressType::Clamp;
};

enum class ShaderEffectPassFlags {
	None = 0,
	PSStyle = 1
};
DEFINE_ENUM_FLAG_OPERATORS(ShaderEffectPassFlags)

struct ShaderEffectPassDesc {
	std::string desc;
	winrt::com_ptr<ID3DBlob> byteCode;
	// 0: INPUT
	// 1: OUTPUT
	// 2+: 中间纹理
	SmallVector<uint32_t> inputs;
	SmallVector<uint32_t> outputs;
	std::array<uint32_t, 3> numThreads{};
	SizeU blockSize{};
	ShaderEffectPassFlags flags = ShaderEffectPassFlags::None;
};

struct ShaderEffectDrawInfo {
	// 不包含 INPUT 和 OUTPUT
	SmallVector<ShaderEffectTextureDesc, 0> textures;
	SmallVector<ShaderEffectSamplerDesc> samplers;
	SmallVector<ShaderEffectPassDesc, 0> passes;
};

}

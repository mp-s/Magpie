#pragma once
#include <d3d11_4.h>
#include <d3d12.h>

namespace Magpie {

struct DirectXHelper {
	union Constant32 {
		float floatVal;
		uint32_t uintVal;
		int intVal;
	};

	static bool IsWARP(const DXGI_ADAPTER_DESC1& desc) noexcept {
		// 不要检查 DXGI_ADAPTER_FLAG_SOFTWARE 标志，如果系统没有安装任何显卡，WARP 没有这个标志。
		// 这两个值来自 https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/d3d10-graphics-programming-guide-dxgi#new-info-about-enumerating-adapters-for-windows-8
		return desc.VendorId == 0x1414 && desc.DeviceId == 0x8c;
	}

	template <size_t Size1, size_t Size2>
	static D3D12_SHADER_BYTECODE SelectShader(
		bool isSM6Supported,
		const uint8_t (&shaderBytes)[Size1],
		const uint8_t (&sm5ShaderBytes)[Size2]
	) noexcept {
		return isSM6Supported ?
			CD3DX12_SHADER_BYTECODE(shaderBytes, Size1) :
			CD3DX12_SHADER_BYTECODE(sm5ShaderBytes, Size2);
	}
};

static inline bool operator==(LUID l, LUID r) noexcept {
	// LowPart 不同的概率更高，因此先检查
	return l.LowPart == r.LowPart && l.HighPart == r.HighPart;
}

}

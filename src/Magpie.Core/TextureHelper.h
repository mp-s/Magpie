#pragma once

namespace Magpie {

class D3D12Context;

struct TextureHelper {
	// 支持 dds、bmp、jpg、png 和 tif。从图片加载纹理时不会执行伽马校正。
	static winrt::com_ptr<ID3D12Resource> LoadFromFile(
		wil::zwstring_view fileName,
		DXGI_FORMAT format,
		const D3D12Context& d3d12Context,
		SizeU& textureSize
	) noexcept;
};

}

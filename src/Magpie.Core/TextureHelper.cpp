#include "pch.h"
#include "D3D12Context.h"
#include "Logger.h"
#include "TextureHelper.h"
#include <wincodec.h>

namespace Magpie {

static winrt::com_ptr<ID3D12Resource> LoadTextureFormImage(
	const wchar_t* fileName,
	DXGI_FORMAT format,
	const D3D12Context& d3d12Context,
	SizeU& textureSize
) noexcept {
	winrt::com_ptr<IWICImagingFactory2> wicImgFactory =
		winrt::try_create_instance<IWICImagingFactory2>(CLSID_WICImagingFactory);
	if (!wicImgFactory) {
		Logger::Get().Error("创建 WICImagingFactory 失败");
		return nullptr;
	}

	// 读取图像文件
	winrt::com_ptr<IWICBitmapDecoder> decoder;
	HRESULT hr = wicImgFactory->CreateDecoderFromFilename(
		fileName, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, decoder.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("IWICImagingFactory::CreateDecoderFromFilename 失败", hr);
		return nullptr;
	}

	winrt::com_ptr<IWICBitmapFrameDecode> frame;
	hr = decoder->GetFrame(0, frame.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("IWICBitmapDecoder::GetFrame 失败", hr);
		return nullptr;
	}

	// 转换格式
	winrt::com_ptr<IWICFormatConverter> formatConverter;
	hr = wicImgFactory->CreateFormatConverter(formatConverter.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("IWICImagingFactory::CreateFormatConverter 失败", hr);
		return nullptr;
	}

	WICPixelFormatGUID targetWicFormat;
	switch (format) {
	case DXGI_FORMAT_R8G8B8A8_UNORM:
		targetWicFormat = GUID_WICPixelFormat32bppRGBA;
		break;
	case DXGI_FORMAT_R10G10B10A2_UNORM:
		targetWicFormat = GUID_WICPixelFormat32bppR10G10B10A2;
		break;
	case DXGI_FORMAT_R16G16B16A16_UNORM:
		targetWicFormat = GUID_WICPixelFormat64bppRGBA;
		break;
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
		targetWicFormat = GUID_WICPixelFormat64bppRGBAHalf;
		break;
	default:
		assert(format == DXGI_FORMAT_R32G32B32A32_FLOAT);
		targetWicFormat = GUID_WICPixelFormat128bppRGBAFloat;
		break;
	}
	
	hr = formatConverter->Initialize(frame.get(), targetWicFormat,
		WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
	if (FAILED(hr)) {
		Logger::Get().ComError("IWICFormatConverter::Initialize 失败", hr);
		return nullptr;
	}

	// 检查 D3D 纹理尺寸限制
	hr = formatConverter->GetSize(&textureSize.width, &textureSize.height);
	if (FAILED(hr)) {
		Logger::Get().ComError("GetSize 失败", hr);
		return nullptr;
	}

	if (textureSize.width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
		textureSize.height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION)
	{
		Logger::Get().Error("图像尺寸超出限制");
		return nullptr;
	}

	winrt::com_ptr<ID3D12Resource> result;

	ID3D12Device5* device = d3d12Context.GetDevice();

	CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);

	D3D12_HEAP_FLAGS heapFlags = d3d12Context.IsHeapFlagCreateNotZeroedSupported() ?
		D3D12_HEAP_FLAG_CREATE_NOT_ZEROED : D3D12_HEAP_FLAG_NONE;

	CD3DX12_RESOURCE_DESC texDesc = CD3DX12_RESOURCE_DESC::Tex2D(
		format, textureSize.width, textureSize.height, 1, 1);

	D3D12_PLACED_SUBRESOURCE_FOOTPRINT textureLayout;
	UINT64 bufferSize;
	device->GetCopyableFootprints(&texDesc, 0, 1, 0,
		&textureLayout, nullptr, nullptr, &bufferSize);

	CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(bufferSize);

	hr = device->CreateCommittedResource(
		&heapProps,
		heapFlags,
		&bufferDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&result)
	);
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateCommittedResource 失败", hr);
		return nullptr;
	}

	CD3DX12_RANGE emptyRange{};
	void* pData;
	hr = result->Map(0, &emptyRange, &pData);
	if (FAILED(hr)) {
		Logger::Get().ComError("ID3D12Resource::Map 失败", hr);
		return nullptr;
	}

	// 这个调用是否会意外读取 pData 的内容？保险起见可以使用临时缓冲区。
	hr = formatConverter->CopyPixels(
		nullptr, textureLayout.Footprint.RowPitch, (UINT)bufferSize, (BYTE*)pData);
	if (FAILED(hr)) {
		Logger::Get().ComError("CopyPixels 失败", hr);
		return nullptr;
	}

	result->Unmap(0, nullptr);

	return std::move(result);
}

winrt::com_ptr<ID3D12Resource> TextureHelper::LoadFromFile(
	wil::zwstring_view fileName,
	DXGI_FORMAT format,
	const D3D12Context& d3d12Context,
	SizeU& textureSize
) noexcept {
	if (fileName.ends_with(L".dds")) {
		return nullptr;
	} else {
		assert(format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R10G10B10A2_UNORM ||
			format == DXGI_FORMAT_R16G16B16A16_UNORM || format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
			format == DXGI_FORMAT_R32G32B32A32_FLOAT);
		return LoadTextureFormImage(fileName.c_str(), format, d3d12Context, textureSize);
	}
}

}

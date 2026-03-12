#include "pch.h"
#include "CursorDrawer.h"
#include "CommandContext.h"
#include "ByteBuffer.h"
#include "ColorHelper.h"
#include "CursorHelper.h"
#include "DescriptorHeap.h"
#include "DirectXHelper.h"
#include "D3D12Context.h"
#include "Logger.h"
#include "ScalingWindow.h"
#include "shaders/CursorResizerPS.h"
#include "shaders/CursorVS.h"
#include "shaders/FullscreenVS.h"
#include "shaders/MaskedCursorPS.h"
#include "shaders/MaskedCursorPS_sRGB.h"
#include "shaders/MonochromeCursorPS.h"
#include "shaders/MonochromeCursorPS_sRGB.h"
#include "shaders/TextureBlitPS.h"
#include "Win32Helper.h"
#include <DirectXPackedVector.h>
#include <ShellScalingApi.h>
#include <wil/registry.h>

namespace Magpie {

using namespace DirectX::PackedVector;

using FnGetCursorFrameInfo = HCURSOR WINAPI(
	HCURSOR hcur,
	LPWSTR  lpName,
	int     iFrame,
	LPDWORD pjifRate,
	LPINT   pccur
);

static FnGetCursorFrameInfo* GetCursorFrameInfo = nullptr;

// 系统 DPI 在程序的生命周期内不会改变，而且使用 GetIconInfo 获得的位图尺寸
// 和此值有关。
static UINT SYSTEM_DPI;

struct StandardCursorInfo {
	HCURSOR handle;
	const wchar_t* regValueName;
	int resId;
};

static std::array<StandardCursorInfo, 16> STANDARD_CURSORS;

static DWORD GetCursorBaseSize() noexcept {
	DWORD cursorBaseSize = 32;

	HRESULT hr = wil::reg::get_value_nothrow(
		HKEY_CURRENT_USER, L"Control Panel\\Cursors", L"CursorBaseSize", &cursorBaseSize);
	// 键不存在不视为错误
	if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
		Logger::Get().ComError("wil::reg::get_value_dword_nothrow 失败", hr);
	}

	return cursorBaseSize;
}

CursorDrawer::~CursorDrawer() noexcept {
#ifdef _DEBUG
	if (_d3d12Context) {
		auto& descriptorHeap = _d3d12Context->GetDescriptorHeap();
		auto& rtvDescriptorHeap = _d3d12Context->GetDescriptorHeap(true);

		for (const auto& pair : _cursorInfos) {
			if (pair.second.textureSrvOffset != std::numeric_limits<uint32_t>::max()) {
				descriptorHeap.Free(pair.second.textureSrvOffset, 1);
			}
			if (pair.second.textureRtvOffset != std::numeric_limits<uint32_t>::max()) {
				rtvDescriptorHeap.Free(pair.second.textureRtvOffset, 1);
			}
			if (pair.second.originTextureSrvOffset != std::numeric_limits<uint32_t>::max()) {
				descriptorHeap.Free(pair.second.originTextureSrvOffset, 1);
			}
		}

		if (_tempOriginTextureSrvOffset != std::numeric_limits<uint32_t>::max()) {
			descriptorHeap.Free(_tempOriginTextureSrvOffset, 1);
		}
		
		for (const _RetiredTempOriginTexture& rt : _retiredTempOriginTextures) {
			if (rt.srvOffset != std::numeric_limits<uint32_t>::max()) {
				descriptorHeap.Free(rt.srvOffset, 1);
			}
		}
	}
#endif
}

bool CursorDrawer::Initialize(
	D3D12Context& d3d12Context,
	const RECT& srcRect,
	const RECT& rendererRect,
	const RECT& destRect,
	const ColorInfo& colorInfo
) noexcept {
	_d3d12Context = &d3d12Context;
	_srcSize.width = uint32_t(srcRect.right - srcRect.left);
	_srcSize.height = uint32_t(srcRect.bottom - srcRect.top);
	_rendererRect = rendererRect;
	_destRect = destRect;
	_colorInfo = colorInfo;

	[[maybe_unused]] static Ignore _ = [] {
		GetCursorFrameInfo = Win32Helper::LoadFunction<FnGetCursorFrameInfo>(
			L"user32.dll", "GetCursorFrameInfo");

		SYSTEM_DPI = GetDpiForSystem();

		// 不包含已废弃的光标, 见 https://learn.microsoft.com/en-us/windows/win32/menurc/about-cursors
		STANDARD_CURSORS[0] = { LoadCursor(NULL, IDC_ARROW), L"Arrow", 0 };
		STANDARD_CURSORS[1] = { LoadCursor(NULL, IDC_IBEAM), L"IBeam", 0 };
		// Wait 和 AppStarting 在“指针大小” 为 1 时是动态光标，否则是静态光标
		STANDARD_CURSORS[2] = { LoadCursor(NULL, IDC_WAIT), L"Wait", 0 };
		STANDARD_CURSORS[3] = { LoadCursor(NULL, IDC_CROSS), L"Crosshair", 0 };
		STANDARD_CURSORS[4] = { LoadCursor(NULL, IDC_UPARROW), L"UpArrow", 0 };
		STANDARD_CURSORS[5] = { LoadCursor(NULL, IDC_SIZENWSE), L"SizeNWSE", 0 };
		STANDARD_CURSORS[6] = { LoadCursor(NULL, IDC_SIZENESW), L"SizeNESW", 0 };
		STANDARD_CURSORS[7] = { LoadCursor(NULL, IDC_SIZEWE), L"SizeWE", 0 };
		STANDARD_CURSORS[8] = { LoadCursor(NULL, IDC_SIZENS), L"SizeNS", 0 };
		STANDARD_CURSORS[9] = { LoadCursor(NULL, IDC_SIZEALL), L"SizeAll", 0 };
		STANDARD_CURSORS[10] = { LoadCursor(NULL, IDC_NO), L"No", 0 };
		STANDARD_CURSORS[11] = { LoadCursor(NULL, IDC_HAND), L"Hand", 0 };
		STANDARD_CURSORS[12] = { LoadCursor(NULL, IDC_APPSTARTING), L"AppStarting", 0 };
		STANDARD_CURSORS[13] = { LoadCursor(NULL, IDC_HELP), L"Help", 0 };
		// Pin 和 Person 只在“指针大小”选项大于 1 时使用注册表中路径，否则使用
		// user32.dll 中的光标资源，ID 分别是 117 和 118。
		STANDARD_CURSORS[14] = { LoadCursor(NULL, IDC_PIN), L"Pin", 117 };
		STANDARD_CURSORS[15] = { LoadCursor(NULL, IDC_PERSON), L"Person", 118 };

		return Ignore();
	}();

	wil::unique_hkey key;
	wil::reg::open_unique_key_nothrow(HKEY_CURRENT_USER, L"Control Panel\\Cursors", key);
	_regWatcher = wil::make_registry_watcher_nothrow(std::move(key), false, [this](wil::RegistryChangeKind) {
		ScalingWindow::Dispatcher().TryEnqueue(
			[this, cursorBaseSize(GetCursorBaseSize()), runId(ScalingWindow::RunId())]() {
			if (ScalingWindow::RunId() != runId || _cursorBaseSize == cursorBaseSize) {
				return;
			}

			_cursorBaseSize = cursorBaseSize;
			// 清空已解析的光标
			_d3d12Context->WaitForGpu();
			_ClearCursorInfos();
		});
	});

	_cursorBaseSize = GetCursorBaseSize();
	return true;
}

bool CursorDrawer::CheckForRedraw(HCURSOR hCursor, POINT cursorPos) noexcept {
	const ScalingWindow& scalingWindow = ScalingWindow::Get();
	const ScalingOptions& options = scalingWindow.Options();

	// 检查自动隐藏光标
	if (options.autoHideCursorDelay.has_value()) {
		using namespace std::chrono;

		// 光标在叠加层上或拖动窗口时禁用自动隐藏。光标处于隐藏状态视为形状不变，考虑形状
		// 变化：箭头->隐藏->箭头，只要位置不变，自动隐藏功能应让光标始终隐藏；反之如果光
		// 标隐藏时移动了或显示时形状变化了应正常显示。
		if (_isCursorVirtualized &&
			!_isMoving &&
			!_isSrcMoving &&
			_curCursorPos == cursorPos &&
			(_lastRawCursorHandle == hCursor || !hCursor)
		) {
			const duration<float> hideDelay(*options.autoHideCursorDelay);
			if (steady_clock::now() - _lastCursorActiveTime > hideDelay) {
				hCursor = NULL;
			}
		} else {
			// 启用自动隐藏时光标形状或位置变化后应记录新的形状、位置和变化时间。位置由
			// _curCursorPos 记录。
			_lastRawCursorHandle = hCursor;
			_lastCursorActiveTime = steady_clock::now();
		}
	}

	if (hCursor) {
		if (_isCursorVisible) {
			static const HCURSOR hArrowCursor = STANDARD_CURSORS[0].handle;

			// 无法解析的光标换为指针光标
			if (!_unresolvableCursors.empty() && _unresolvableCursors.contains(hCursor)) {
				if (hCursor != hArrowCursor && !_unresolvableCursors.contains(hArrowCursor)) {
					hCursor = hArrowCursor;
				} else {
					hCursor = NULL;
				}
			}

			if (hCursor) {
				while (true) {
					_curCursorInfo = _ResolveCursor(hCursor, cursorPos, false);
					if (_curCursorInfo) {
						// 检查光标是否在视口内
						const RECT cursorRect = {
							cursorPos.x - (LONG)_curCursorInfo->hotspot.x,
							cursorPos.y - (LONG)_curCursorInfo->hotspot.y,
							cursorRect.left + (LONG)_curCursorInfo->size.width,
							cursorRect.top + (LONG)_curCursorInfo->size.height
						};
						if (!Win32Helper::IsRectOverlap(cursorRect, _destRect)) {
							hCursor = NULL;
						}

						break;
					} else {
						_unresolvableCursors.insert(hCursor);

						if (hCursor == hArrowCursor) {
							// 无法解析指针光标
							hCursor = NULL;
							break;
						} else {
							// 换为指针光标
							hCursor = hArrowCursor;
						}
					}
				}
			}
		} else {
			// 截屏时暂时不渲染光标
			hCursor = NULL;
		}
	}

	// 光标形状或位置变化时需要重新绘制
	if (hCursor != _hCurCursor || (hCursor && cursorPos != _curCursorPos)) {
		_hCurCursor = hCursor;
		_curCursorPos = cursorPos;
		return true;
	} else {
		return false;
	}
}

HRESULT CursorDrawer::Draw(
	GraphicsContext& graphicsContext,
	uint64_t completedFenceValue,
	uint64_t nextFenceValue,
	uint32_t curFrameSrvOffset,
	ID3D12Resource* backBuffer
) noexcept {
	_ClearRetiredResources(completedFenceValue);

	if (!_hCurCursor || !_curCursorInfo) {
		return S_OK;
	}

	if (!_curCursorInfo->texture) {
		HRESULT hr = _InitializeCursorTexture(graphicsContext , *_curCursorInfo);
		if (FAILED(hr)) {
			Logger::Get().ComError("_InitializeCursorTexture 失败", hr);
			return hr;
		}
	}

	const RECT cursorRect = {
		.left = _curCursorPos.x - (LONG)_curCursorInfo->hotspot.x,
		.top = _curCursorPos.y - (LONG)_curCursorInfo->hotspot.y,
		.right = cursorRect.left + (LONG)_curCursorInfo->size.width,
		.bottom = cursorRect.top + (LONG)_curCursorInfo->size.height
	};

	const bool isSrgb = _colorInfo.kind == winrt::AdvancedColorKind::StandardDynamicRange;
	
	if (_curCursorInfo->type == _CursorType::Color) {
		winrt::com_ptr<ID3D12PipelineState>& pso = isSrgb ? _colorSrgbPSO : _colorPSO;
		if (!pso) {
			HRESULT hr = _CreateColorPSO(isSrgb, pso);
			if (FAILED(hr)) {
				Logger::Get().ComError("_CreateColorPSO 失败", hr);
				return hr;
			}
		}

		graphicsContext.SetPipelineState(pso.get());
		graphicsContext.SetRootSignature(_colorRootSignature.get());
	} else {
		bool isMonochrome = _curCursorInfo->type == _CursorType::Monochrome;
		
		winrt::com_ptr<ID3D12PipelineState>& pso = isMonochrome ?
			(isSrgb ? _monochromeSrgbPSO : _monochromePSO) :
			(isSrgb ? _maskedColorSrgbPSO : _maskedColorPSO);

		if (!pso) {
			HRESULT hr = _CreateMaskPSO(isMonochrome, isSrgb, pso);
			if (FAILED(hr)) {
				Logger::Get().ComError("_CreateMaskPSO 失败", hr);
				return hr;
			}
		}

		graphicsContext.SetPipelineState(pso.get());
		graphicsContext.SetRootSignature(_maskRootSignature.get());
	}

	const RECT viewportRect = {
		_destRect.left - _rendererRect.left,
		_destRect.top - _rendererRect.top,
		_destRect.right - _rendererRect.left,
		_destRect.bottom - _rendererRect.top
	};
	const SIZE viewportSize = Win32Helper::GetSizeOfRect(viewportRect);

	{
		// 转换到 NDC
		float constants[] = {
			(cursorRect.left - _destRect.left) * 2 / (float)viewportSize.cx - 1.0f,	// left
			1.0f - (cursorRect.top - _destRect.top) * 2 / (float)viewportSize.cy,	// top
			_curCursorInfo->size.width * 2 / (float)viewportSize.cx,	// width
			_curCursorInfo->size.height * 2 / (float)-viewportSize.cy	// height
		};
		graphicsContext.SetRoot32BitConstants(0, (UINT)std::size(constants), constants);
	}

	if (_curCursorInfo->type == _CursorType::Color) {
		graphicsContext.SetRootDescriptorTable(1, _curCursorInfo->textureSrvOffset);
	} else {
		DirectXHelper::Constant32 constants[] = {
			{.uintVal = _curCursorInfo->originSize.width},
			{.uintVal = _curCursorInfo->originSize.height},
			{.uintVal = _curCursorInfo->size.width},
			{.uintVal = _curCursorInfo->size.height},
			{.uintVal = backBuffer ? 0u :uint32_t(cursorRect.left - _destRect.left)},
			{.uintVal = backBuffer ? 0u : uint32_t(cursorRect.top - _destRect.top)},
			// 原始帧需要伽马校正，从渲染目标复制的临时纹理不需要。WCG/HDR 下这个参数有不同的意义
			{.uintVal = isSrgb ? (backBuffer ? 0u : 1u) :
				std::bit_cast<uint32_t>(_colorInfo.sdrWhiteLevel)}
		};
		graphicsContext.SetRoot32BitConstants(1, (UINT)std::size(constants), constants);

		graphicsContext.SetRootDescriptorTable(2, _curCursorInfo->textureSrvOffset);

		if (backBuffer) {
			// 掩码光标在叠加层上时需要从渲染目标复制光标下区域到临时纹理
			if (_tempOriginTextureSize.width < _curCursorInfo->size.width ||
				_tempOriginTextureSize.height < _curCursorInfo->size.height
			) {
				if (_tempOriginTexture) {
					_retiredTempOriginTextures.emplace_back(_RetiredTempOriginTexture{
						.texture = std::move(_tempOriginTexture),
						.fenceValue = nextFenceValue,
						.srvOffset = _tempOriginTextureSrvOffset
					});
				}

				// 对齐到 32 的倍数
				_tempOriginTextureSize.width = (_curCursorInfo->size.width + 31) & ~31;
				_tempOriginTextureSize.height = (_curCursorInfo->size.height + 31) & ~31;

				ID3D12Device5* device = _d3d12Context->GetDevice();

				CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);

				CD3DX12_RESOURCE_DESC texDesc = CD3DX12_RESOURCE_DESC::Tex2D(
					isSrgb ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT,
					_tempOriginTextureSize.width,
					_tempOriginTextureSize.height,
					1, 1, 1, 0,
					D3D12_RESOURCE_FLAG_NONE
				);

				HRESULT hr = device->CreateCommittedResource(
					&heapProps,
					D3D12_HEAP_FLAG_CREATE_NOT_ZEROED,
					&texDesc,
					D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
					nullptr,
					IID_PPV_ARGS(&_tempOriginTexture)
				);
				if (FAILED(hr)) {
					Logger::Get().ComError("CreateCommittedResource 失败", hr);
					return hr;
				}
				
				auto& descriptorHeap = _d3d12Context->GetDescriptorHeap();

				hr = descriptorHeap.Alloc(1, _tempOriginTextureSrvOffset);
				if (FAILED(hr)) {
					Logger::Get().ComError("DescriptorHeap::Alloc 失败", hr);
					return hr;
				}

				CD3DX12_SHADER_RESOURCE_VIEW_DESC srvDesc =
					CD3DX12_SHADER_RESOURCE_VIEW_DESC::Tex2D(texDesc.Format, 1);
				device->CreateShaderResourceView(_tempOriginTexture.get(), &srvDesc,
					descriptorHeap.GetCpuHandle(_tempOriginTextureSrvOffset));
			}

			graphicsContext.InsertTransitionBarrier(
				_tempOriginTexture.get(),
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_COPY_DEST
			);
			graphicsContext.InsertTransitionBarrier(
				backBuffer,
				D3D12_RESOURCE_STATE_RENDER_TARGET,
				D3D12_RESOURCE_STATE_COPY_SOURCE
			);
			
			{
				D3D12_BOX srcBox = {
					UINT(std::max(cursorRect.left - _rendererRect.left, viewportRect.left)),
					UINT(std::max(cursorRect.top - _rendererRect.top, viewportRect.top)),
					0,
					UINT(std::min(cursorRect.right - _rendererRect.left, viewportRect.right)),
					UINT(std::min(cursorRect.bottom - _rendererRect.top, viewportRect.bottom)),
					1
				};
				uint32_t destLeft = uint32_t(std::max(0l,
					viewportRect.left - cursorRect.left + _rendererRect.left));
				uint32_t destTop = uint32_t(std::max(0l,
					viewportRect.top - cursorRect.top + _rendererRect.top));

				assert(destLeft + srcBox.right - srcBox.left <= _curCursorInfo->size.width);
				assert(destTop + srcBox.bottom - srcBox.top <= _curCursorInfo->size.height);

				graphicsContext.CopyTextureRegion(
					_tempOriginTexture.get(), destLeft, destTop, backBuffer, &srcBox);
			}
			
			graphicsContext.InsertTransitionBarrier(
				_tempOriginTexture.get(),
				D3D12_RESOURCE_STATE_COPY_DEST,
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
			);
			graphicsContext.InsertTransitionBarrier(
				backBuffer,
				D3D12_RESOURCE_STATE_COPY_SOURCE,
				D3D12_RESOURCE_STATE_RENDER_TARGET
			);

			graphicsContext.SetRootDescriptorTable(3, _tempOriginTextureSrvOffset);
		} else {
			// 直接使用原始帧
			graphicsContext.SetRootDescriptorTable(3, curFrameSrvOffset);
		}
	}

	graphicsContext.RSSetViewportAndScissorRect(viewportRect);
	
	graphicsContext.Draw(4);
	
	return S_OK;
}

void CursorDrawer::OnMoved(const RECT& rendererRect, const RECT& destRect) noexcept {
	_rendererRect = rendererRect;
	_destRect = destRect;
}

void CursorDrawer::OnResized(const RECT& rendererRect, const RECT& destRect) noexcept {
	_rendererRect = rendererRect;
	_destRect = destRect;

	// 光标缩放和源窗口相同则清空已解析的光标
	if (ScalingWindow::Get().Options().cursorScale < FLOAT_EPSILON<float>) {
		_ClearCursorInfos();
	}
}

void CursorDrawer::OnColorInfoChanged(const ColorInfo& colorInfo) noexcept {
	bool wasScRGB = _colorInfo.kind != winrt::AdvancedColorKind::StandardDynamicRange;
	bool isScRGB = colorInfo.kind != winrt::AdvancedColorKind::StandardDynamicRange;
	_colorInfo = colorInfo;

	if (wasScRGB != isScRGB) {
		// 渲染目标纹理格式变了
		_tempOriginTexture = nullptr;
		_tempOriginTextureSize = {};

		if (_tempOriginTextureSrvOffset != std::numeric_limits<uint32_t>::max()) {
			_d3d12Context->GetDescriptorHeap().Free(_tempOriginTextureSrvOffset, 1);
			_tempOriginTextureSrvOffset = std::numeric_limits<uint32_t>::max();
		}
	}

	_ClearCursorInfos();
}

static bool GetCursorSizeFromBmps(HBITMAP hColorBmp, HBITMAP hMaskBmp, Size& size) noexcept {
	BITMAP bmp{};
	if (!GetObject(hColorBmp ? hColorBmp : hMaskBmp, sizeof(bmp), &bmp)) {
		Logger::Get().Win32Error("GetObject 失败");
		return false;
	}

	size.width = uint32_t(bmp.bmWidth);
	// 单色光标的掩码位图高度是光标实际高度的两倍
	size.height = uint32_t(hColorBmp ? bmp.bmHeight : bmp.bmHeight / 2);
	return true;
}

CursorDrawer::_CursorInfo* CursorDrawer::_ResolveCursor(
	HCURSOR hCursor,
	POINT cursorPos,
	bool isAni
) noexcept {
	assert(hCursor);

	/*if (GetCursorFrameInfo) {
		DWORD jifRate;
		int stepCount;
		HCURSOR hTmpCursor = GetCursorFrameInfo(hCursor, nullptr, 0, &jifRate, &stepCount);
		if (hTmpCursor && hTmpCursor != hCursor) {
			hCursor = hTmpCursor;
		}
	}*/

	// 检索光标所在屏幕的 DPI
	const HMONITOR hCurMon = MonitorFromPoint(
		{ cursorPos.x + _destRect.left, cursorPos.y + _destRect.top }, MONITOR_DEFAULTTOPRIMARY);
	UINT monitorDpi = USER_DEFAULT_SCREEN_DPI;
	GetDpiForMonitor(hCurMon, MDT_EFFECTIVE_DPI, &monitorDpi, &monitorDpi);
	
	auto it = _cursorInfos.find(std::make_pair(hCursor, monitorDpi));
	if (it != _cursorInfos.end()) {
		return &it->second;
	}

	// 检查此光标是否不随 DPI 缩放
	it = _cursorInfos.find(std::make_pair(hCursor, 0));
	if (it != _cursorInfos.end()) {
		return &it->second;
	}

	_CursorInfo cursorInfo{};
	// 如果不能确定光标是否随 DPI 缩放则假设为真，绝大多数情况下是对的
	bool isCursorDpiAware = !isAni;

	ICONINFOEX iconInfoEx = { .cbSize = sizeof(iconInfoEx) };
	if (!GetIconInfoEx(hCursor, &iconInfoEx)) {
		Logger::Get().Win32Error("GetIconInfoEx 失败");
		return nullptr;
	}

	wil::unique_hbitmap hColorBmp(iconInfoEx.hbmColor);
	wil::unique_hbitmap hMaskBmp(iconInfoEx.hbmMask);

	if (!GetCursorSizeFromBmps(hColorBmp.get(), hMaskBmp.get(), cursorInfo.originSize)) {
		Logger::Get().Error("GetCursorSizeFromBmps 失败");
		return nullptr;
	}

	Point originHotspot = { iconInfoEx.xHotspot, iconInfoEx.yHotspot };
	
	// 将线程 DPI 感知设为 unaware 后 GetIconInfo 可以获得 100% DPI 缩放下的光标位图。
	// 我们借助这个特性检查光标是否随 DPI 缩放，不过只在程序启动时系统 DPI 缩放不是
	// 100% 时有效。
	if (SYSTEM_DPI == 96) {
		cursorInfo.size = _CalcCursorSize(
			cursorInfo.originSize, SYSTEM_DPI, monitorDpi);
	} else {
		ICONINFO iconInfoDpi96{};
		{
			DPI_AWARENESS_CONTEXT oldDpiContext =
				SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_UNAWARE);
			auto se = wil::scope_exit([&] {
				SetThreadDpiAwarenessContext(oldDpiContext);
			});

			if (!GetIconInfo(hCursor, &iconInfoDpi96)) {
				Logger::Get().Win32Error("GetIconInfo 失败");
				return nullptr;
			}
		}

		wil::unique_hbitmap hColorBmpDpi96(iconInfoDpi96.hbmColor);
		wil::unique_hbitmap hMaskBmpDpi96(iconInfoDpi96.hbmMask);

		Size bmpSizeDpi96;
		if (!GetCursorSizeFromBmps(hColorBmpDpi96.get(), hMaskBmpDpi96.get(), bmpSizeDpi96)) {
			Logger::Get().Error("GetCursorSizeFromBmps 失败");
			return nullptr;
		}

		// 不同 DPI 下光标位图尺寸不变说明光标不跟随 DPI 缩放
		if (cursorInfo.originSize == bmpSizeDpi96) {
			isCursorDpiAware = false;
			cursorInfo.size = bmpSizeDpi96;
		} else {
			cursorInfo.size = _CalcCursorSize(bmpSizeDpi96, 96, monitorDpi);
		}

		if (cursorInfo.size == bmpSizeDpi96) {
			hColorBmp = std::move(hColorBmpDpi96);
			hMaskBmp = std::move(hMaskBmpDpi96);
			cursorInfo.originSize = bmpSizeDpi96;
			originHotspot = { iconInfoDpi96.xHotspot,iconInfoDpi96.yHotspot };
		}
	}

	wil::unique_hcursor hResCursor;

	const auto it1 = std::find_if(
		STANDARD_CURSORS.begin(),
		STANDARD_CURSORS.end(),
		[&](const StandardCursorInfo& info) {
			return info.handle == hCursor;
		}
	);
	if (it1 == STANDARD_CURSORS.end()) {
		hResCursor = _TryResolveCursorResource(iconInfoEx, cursorInfo.size.width);
	} else {
		hResCursor = _TryResolveStandardCursor(it1->regValueName, it1->resId, cursorInfo.size.width);
	}

	if (hResCursor) {
		ICONINFO iconInfo{};
		if (!GetIconInfo(hResCursor.get(), &iconInfo)) {
			Logger::Get().Win32Error("GetIconInfo 失败");
			return nullptr;
		}

		hColorBmp.reset(iconInfo.hbmColor);
		hMaskBmp.reset(iconInfo.hbmMask);
		if (!GetCursorSizeFromBmps(hColorBmp.get(), hMaskBmp.get(), cursorInfo.originSize)) {
			Logger::Get().Error("GetCursorSizeFromBmps 失败");
			return nullptr;
		}

		originHotspot = { iconInfo.xHotspot,iconInfo.yHotspot };
	}

	cursorInfo.hotspot.x = (uint32_t)lround(
		originHotspot.x * cursorInfo.size.width / (double)cursorInfo.originSize.width);
	cursorInfo.hotspot.y = (uint32_t)lround(
		originHotspot.y * cursorInfo.size.height / (double)cursorInfo.originSize.height);

	if (!_ResolveCursorPixels(cursorInfo, hColorBmp.get(), hMaskBmp.get())) {
		Logger::Get().Error("_ResolveCursorPixels 失败");
		return nullptr;
	}

	return &_cursorInfos.emplace(std::make_pair(hCursor, isCursorDpiAware ? monitorDpi : 0),
		std::move(cursorInfo)).first->second;
}

Size CursorDrawer::_CalcCursorSize(
	Size cursorBmpSize,
	uint32_t cursorDpi,
	uint32_t monitorDpi
) const noexcept {
	float scale = ScalingWindow::Get().Options().cursorScale;
	if (scale < FLOAT_EPSILON<float>) {
		// 光标缩放和源窗口相同
		SIZE destSize = Win32Helper::GetSizeOfRect(_destRect);
		scale = (((float)destSize.cx / _srcSize.width) +
			((float)destSize.cy / _srcSize.height)) / 2;
	}

	scale *= (GetSystemMetricsForDpi(SM_CXCURSOR, monitorDpi) * _cursorBaseSize) /
		(GetSystemMetricsForDpi(SM_CXCURSOR, cursorDpi) * 32.0f);

	return { (uint32_t)std::lroundf(cursorBmpSize.width * scale),
		(uint32_t)std::lroundf(cursorBmpSize.height * scale) };
}

wil::unique_hcursor CursorDrawer::_TryResolveCursorResource(
	const ICONINFOEX& iconInfoEx,
	uint32_t preferedWidth
) const noexcept {
	wil::unique_hcursor result;

	if (StrHelper::StrLen(iconInfoEx.szModName) == 0) {
		return result;
	}

	HMODULE hModule = LoadLibraryEx(iconInfoEx.szModName, NULL, LOAD_LIBRARY_AS_DATAFILE);
	if (hModule) {
		LPCWSTR resName = iconInfoEx.wResID != 0 ?
			MAKEINTRESOURCE(iconInfoEx.wResID) : iconInfoEx.szResName;
		result = CursorHelper::ExtractCursorFromModule(hModule, resName, preferedWidth);
		FreeLibrary(hModule);
	} else {
		Logger::Get().Win32Error("LoadLibraryEx 失败");
	}
	
	return result;
}

wil::unique_hcursor CursorDrawer::_TryResolveStandardCursor(
	const wchar_t* regValueName,
	int resId,
	uint32_t preferedWidth
) const noexcept {
	wil::unique_hcursor result;

	if (_cursorBaseSize == 32 && resId != 0) {
		HMODULE hUser32 = GetModuleHandle(L"user32.dll");
		result = CursorHelper::ExtractCursorFromModule(
			hUser32, MAKEINTRESOURCE(resId), preferedWidth);
		if (!result) {
			Logger::Get().Error("CursorHelper::ExtractCursorFromModule 失败");
		}
	} else {
		wil::unique_bstr regValue;
		HRESULT hr = wil::reg::get_value_nothrow(
			HKEY_CURRENT_USER, L"Control Panel\\Cursors", regValueName, &regValue);
		// 失败不视为错误
		if (FAILED(hr)) {
			return result;
		}

		// 可能为空
		if (SysStringLen(regValue.get()) == 0) {
			return result;
		}

		// 路径中可能包含环境变量字符串
		std::wstring curPath;
		hr = wil::ExpandEnvironmentStringsW(regValue.get(), curPath);
		if (FAILED(hr)) {
			Logger::Get().ComError("wil::ExpandEnvironmentStringsW 失败", hr);
			return result;
		}

		result = CursorHelper::ExtractCursorFromCurFile(curPath.c_str(), preferedWidth);
		if (!result) {
			Logger::Get().Error("CursorHelper::ExtractCursorFromCurFile 失败");
		}
	}

	return result;
}

static void FillColorCursorRGB(
	HALF* textureData,
	uint8_t* pixels,
	const ColorInfo& colorInfo,
	float alpha
) noexcept {
	// 预乘 Alpha 通道并归一化
	float factor = alpha / 255.0f;

	if (colorInfo.kind == winrt::AdvancedColorKind::StandardDynamicRange) {
		*textureData++ = XMConvertFloatToHalf(pixels[2] * factor);
		*textureData++ = XMConvertFloatToHalf(pixels[1] * factor);
		*textureData = XMConvertFloatToHalf(pixels[0] * factor);
	} else {
		if (colorInfo.kind == winrt::AdvancedColorKind::HighDynamicRange) {
			factor *= colorInfo.sdrWhiteLevel;
		}

		*textureData++ = XMConvertFloatToHalf(ColorHelper::SrgbToLinear(pixels[2]) * factor);
		*textureData++ = XMConvertFloatToHalf(ColorHelper::SrgbToLinear(pixels[1]) * factor);
		*textureData = XMConvertFloatToHalf(ColorHelper::SrgbToLinear(pixels[0]) * factor);
	}
}

bool CursorDrawer::_ResolveCursorPixels(
	_CursorInfo& cursorInfo,
	HBITMAP hColorBmp,
	HBITMAP hMaskBmp
) const noexcept {
	const Size bmpSize = {
		cursorInfo.originSize.width,
		hColorBmp ? cursorInfo.originSize.height : cursorInfo.originSize.height * 2
	};

	// 单色光标也转换成 32 位以方便处理
	BITMAPINFO bi = {
		.bmiHeader = {
			.biSize = sizeof(BITMAPINFOHEADER),
			.biWidth = (LONG)bmpSize.width,
			.biHeight = -(LONG)bmpSize.height,
			.biPlanes = 1,
			.biBitCount = 32,
			.biCompression = BI_RGB,
			.biSizeImage = DWORD(bmpSize.width * bmpSize.height * 4)
		}
	};

	ByteBuffer pixels(bi.bmiHeader.biSizeImage);
	wil::unique_hdc_window hdcScreen(wil::window_dc(GetDC(NULL)));
	if (GetDIBits(hdcScreen.get(), hColorBmp ? hColorBmp : hMaskBmp, 0, bmpSize.height,
		pixels.Data(), &bi, DIB_RGB_COLORS) != (int)bmpSize.height
	) {
		Logger::Get().Win32Error("GetDIBits 失败");
		return false;
	}

	if (hColorBmp) {
		// 彩色掩码光标和彩色光标的区别在于前者的透明通道全为 0
		bool hasAlpha = false;
		for (uint32_t i = 3; i < bi.bmiHeader.biSizeImage; i += 4) {
			if (pixels[i] != 0) {
				hasAlpha = true;
				break;
			}
		}

		if (hasAlpha) {
			// 彩色光标
			cursorInfo.type = _CursorType::Color;
			cursorInfo.originTextureData.Resize(bi.bmiHeader.biSizeImage * 2);

			HALF* textureData = (HALF*)cursorInfo.originTextureData.Data();

			for (uint32_t i = 0; i < bi.bmiHeader.biSizeImage; i += 4) {
				float alpha = pixels[i + 3] / 255.0f;
				FillColorCursorRGB(&textureData[i], &pixels[i], _colorInfo, alpha);
				textureData[i + 3] = XMConvertFloatToHalf(1.0f - alpha);
			}
		} else {
			// 彩色掩码光标。如果不需要应用 XOR 则转换成彩色光标
			ByteBuffer maskPixels(bi.bmiHeader.biSizeImage);

			if (GetDIBits(hdcScreen.get(), hMaskBmp, 0, bmpSize.height,
				maskPixels.Data(), &bi, DIB_RGB_COLORS) != (int)bmpSize.height
			) {
				Logger::Get().Win32Error("GetDIBits 失败");
				return false;
			}

			// 计算此彩色掩码光标是否可以转换为彩色光标
			bool canConvertToColor = true;
			for (uint32_t i = 0; i < bi.bmiHeader.biSizeImage; i += 4) {
				if (maskPixels[i] != 0 &&
					(pixels[i] != 0 || pixels[i + 1] != 0 || pixels[i + 2] != 0)
				) {
					// 掩码不为 0 则不能转换为彩色光标
					canConvertToColor = false;
					break;
				}
			}

			if (canConvertToColor) {
				// 转换为彩色光标以获得更好的插值效果和渲染性能
				cursorInfo.type = _CursorType::Color;
				cursorInfo.originTextureData.Resize(bi.bmiHeader.biSizeImage * 2);

				HALF* textureData = (HALF*)cursorInfo.originTextureData.Data();

				for (uint32_t i = 0; i < bi.bmiHeader.biSizeImage; i += 4) {
					if (maskPixels[i] == 0) {
						// Alpha 通道为 0，无需设置
						FillColorCursorRGB(&textureData[i], &pixels[i], _colorInfo, 1.0f);
					} else {
						// 透明像素
						textureData[i + 3] = XMConvertFloatToHalf(1.0f);
					}
				}
			} else {
				cursorInfo.type = _CursorType::MaskedColor;

				// 将 XOR 掩码复制到透明通道中
				for (uint32_t i = 0; i < bi.bmiHeader.biSizeImage; i += 4) {
					std::swap(pixels[i], pixels[i + 2]);
					pixels[i + 3] = maskPixels[i];
				}

				cursorInfo.originTextureData = std::move(pixels);
			}
		}
	} else {
		// 单色光标。如果不需要应用反色则转换成彩色光标
		const uint32_t halfSize = bi.bmiHeader.biSizeImage / 2;

		// 计算此单色光标是否可以转换为彩色光标
		bool canConvertToColor = true;
		for (uint32_t i = 0; i < halfSize; i += 4) {
			// 上半部分是 AND 掩码，下半部分是 XOR 掩码
			if (pixels[i] != 0 && pixels[i + halfSize] != 0) {
				canConvertToColor = false;
				break;
			}
		}
		
		if (canConvertToColor) {
			// 转换为彩色光标以获得更好的插值效果和渲染性能
			cursorInfo.type = _CursorType::Color;
			cursorInfo.originTextureData.Resize(bi.bmiHeader.biSizeImage * 2);

			HALF* textureData = (HALF*)cursorInfo.originTextureData.Data();

			for (uint32_t i = 0; i < halfSize; i += 4) {
				// 上半部分是 AND 掩码，下半部分是 XOR 掩码
				// https://learn.microsoft.com/en-us/windows-hardware/drivers/display/drawing-monochrome-pointers
				if (pixels[i] == 0) {
					// 黑色全为 0，无需设置
					if (pixels[i + halfSize] != 0) {
						// 白色。非 HDR 时 sdrWhiteLevel 始终为 1
						std::fill_n(&textureData[i], 3, XMConvertFloatToHalf(_colorInfo.sdrWhiteLevel));
					}
				} else {
					// 透明
					textureData[i + 3] = XMConvertFloatToHalf(1.0f);
				}
			}
		} else {
			cursorInfo.type = _CursorType::Monochrome;

			// 位图数据：上半部分是 AND 掩码，下半部分是 XOR 掩码
			// 纹理数据：高四位是 AND 掩码，低四位是 XOR 掩码
			uint8_t* upperPtr = &pixels[0];
			uint8_t* lowerPtr = &pixels[halfSize];
			uint8_t* targetPtr = &pixels[0];
			for (uint32_t i = 0; i < halfSize; i += 4) {
				*targetPtr++ = (*upperPtr & 0xf0) | (*lowerPtr & 0xf);

				upperPtr += 4;
				lowerPtr += 4;
			}

			cursorInfo.originTextureData = std::move(pixels);
		}
	}

	return true;
}

HRESULT CursorDrawer::_InitializeCursorTexture(
	GraphicsContext& graphicsContext,
	_CursorInfo& cursorInfo
) noexcept {
	ID3D12Device5* device = _d3d12Context->GetDevice();

	CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);

	CD3DX12_RESOURCE_DESC texDesc = CD3DX12_RESOURCE_DESC::Tex2D(
		cursorInfo.type == _CursorType::Color ? DXGI_FORMAT_R16G16B16A16_FLOAT :
			(cursorInfo.type == _CursorType::Monochrome ? DXGI_FORMAT_R8_UINT : DXGI_FORMAT_R8G8B8A8_UNORM),
		cursorInfo.originSize.width, cursorInfo.originSize.height, 1, 1);

	D3D12_PLACED_SUBRESOURCE_FOOTPRINT textureLayout;
	UINT64 textureRowSizeInBytes;
	UINT64 textureSize;
	device->GetCopyableFootprints(&texDesc, 0, 1, 0,
		&textureLayout, nullptr, &textureRowSizeInBytes, &textureSize);

	assert(textureRowSizeInBytes == cursorInfo.originSize.width *
		(cursorInfo.type == _CursorType::Color ? 8 : (cursorInfo.type == _CursorType::Monochrome ? 1 : 4)));

	CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(textureSize);

	HRESULT hr = device->CreateCommittedResource(
		&heapProps,
		D3D12_HEAP_FLAG_CREATE_NOT_ZEROED,
		&bufferDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&cursorInfo.originUploadBuffer)
	);
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateCommittedResource 失败", hr);
		return hr;
	}

	heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
	hr = device->CreateCommittedResource(
		&heapProps,
		D3D12_HEAP_FLAG_CREATE_NOT_ZEROED,
		&texDesc,
		D3D12_RESOURCE_STATE_COPY_DEST,
		nullptr,
		IID_PPV_ARGS(&cursorInfo.originTexture)
	);
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateCommittedResource 失败", hr);
		return hr;
	}

	CD3DX12_RANGE emptyRange{};
	void* pData;
	hr = cursorInfo.originUploadBuffer->Map(0, &emptyRange, &pData);
	if (FAILED(hr)) {
		Logger::Get().ComError("ID3D12Resource::Map 失败", hr);
		return hr;
	}

	if (textureRowSizeInBytes == textureLayout.Footprint.RowPitch) {
		std::memcpy(pData, cursorInfo.originTextureData.Data(), textureRowSizeInBytes * cursorInfo.originSize.height);
	} else {
		for (uint32_t i = 0; i < cursorInfo.originSize.height; ++i) {
			std::memcpy(
				(uint8_t*)pData + textureLayout.Footprint.RowPitch * i,
				&cursorInfo.originTextureData[(uint32_t)textureRowSizeInBytes * i],
				textureRowSizeInBytes
			);
		}
	}

	cursorInfo.originUploadBuffer->Unmap(0, nullptr);

	cursorInfo.originTextureData.Clear();

	graphicsContext.CopyTextureRegion(
		CD3DX12_TEXTURE_COPY_LOCATION(cursorInfo.originTexture.get()),
		0,
		0,
		CD3DX12_TEXTURE_COPY_LOCATION(cursorInfo.originUploadBuffer.get(), textureLayout)
	);

	graphicsContext.InsertTransitionBarrier(
		cursorInfo.originTexture.get(),
		D3D12_RESOURCE_STATE_COPY_DEST,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
	);

	auto& descriptorHeap = _d3d12Context->GetDescriptorHeap();

	// 单色光标和彩色掩码光标始终使用最近邻采样，无需缩放
	if (cursorInfo.type != _CursorType::Color || cursorInfo.size == cursorInfo.originSize) {
		cursorInfo.texture = std::move(cursorInfo.originTexture);
	} else {
		texDesc.Width = cursorInfo.size.width;
		texDesc.Height = cursorInfo.size.height;
		texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

		hr = device->CreateCommittedResource(
			&heapProps,
			D3D12_HEAP_FLAG_CREATE_NOT_ZEROED,
			&texDesc,
			D3D12_RESOURCE_STATE_RENDER_TARGET,
			nullptr,
			IID_PPV_ARGS(&cursorInfo.texture)
		);
		if (FAILED(hr)) {
			Logger::Get().ComError("CreateCommittedResource 失败", hr);
			return hr;
		}

		// 以 D3D12_HEAP_FLAG_CREATE_NOT_ZEROED 创建的资源作为渲染目标需先
		// Discard/Clear/Copy。
		graphicsContext.DiscardResource(cursorInfo.texture.get());

		auto& rtvDescriptorHeap = _d3d12Context->GetDescriptorHeap(true);

		hr = descriptorHeap.Alloc(1, cursorInfo.originTextureSrvOffset);
		if (FAILED(hr)) {
			Logger::Get().ComError("DescriptorHeap::Alloc 失败", hr);
			return hr;
		}

		hr = rtvDescriptorHeap.Alloc(1, cursorInfo.textureRtvOffset);
		if (FAILED(hr)) {
			Logger::Get().ComError("DescriptorHeap::Alloc 失败", hr);
			return hr;
		}

		{
			CD3DX12_SHADER_RESOURCE_VIEW_DESC srvDesc =
				CD3DX12_SHADER_RESOURCE_VIEW_DESC::Tex2D(texDesc.Format, 1);
			device->CreateShaderResourceView(
				cursorInfo.originTexture.get(),
				&srvDesc,
				descriptorHeap.GetCpuHandle(cursorInfo.originTextureSrvOffset)
			);
		}

		{
			D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {
				.Format = texDesc.Format,
				.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D
			};
			device->CreateRenderTargetView(
				cursorInfo.texture.get(),
				&rtvDesc,
				rtvDescriptorHeap.GetCpuHandle(cursorInfo.textureRtvOffset)
			);
		}

		if (!_cursorResizerPSO) {
			hr = _CreateCursorResizerPSO();
			if (FAILED(hr)) {
				Logger::Get().ComError("_CreateCursorResizerPSO 失败", hr);
				return hr;
			}
		}
		
		graphicsContext.SetPipelineState(_cursorResizerPSO.get());
		graphicsContext.SetRootSignature(_cursorResizerRootSignature.get());

		{
			DirectXHelper::Constant32 constants[] = {
				{.uintVal = cursorInfo.originSize.width},
				{.uintVal = cursorInfo.originSize.height},
				{.floatVal = 1.0f / cursorInfo.originSize.width},
				{.floatVal = 1.0f / cursorInfo.originSize.height}
			};
			graphicsContext.SetRoot32BitConstants(0, (UINT)std::size(constants), constants);
		}

		graphicsContext.SetRootDescriptorTable(1, cursorInfo.originTextureSrvOffset);

		graphicsContext.RSSetViewportAndScissorRect(CD3DX12_RECT(
			0, 0, (LONG)cursorInfo.size.width, (LONG)cursorInfo.size.height));

		uint32_t oldRtvOffset = graphicsContext.OMGetRenderTarget();
		graphicsContext.OMSetRenderTarget(_curCursorInfo->textureRtvOffset);
		
		graphicsContext.Draw(3);

		// 还原渲染目标
		graphicsContext.OMSetRenderTarget(oldRtvOffset);

		graphicsContext.InsertTransitionBarrier(
			cursorInfo.texture.get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
		);
	}

	hr = descriptorHeap.Alloc(1, cursorInfo.textureSrvOffset);
	if (FAILED(hr)) {
		Logger::Get().ComError("DescriptorHeap::Alloc 失败", hr);
		return hr;
	}

	CD3DX12_SHADER_RESOURCE_VIEW_DESC srvDesc =
		CD3DX12_SHADER_RESOURCE_VIEW_DESC::Tex2D(texDesc.Format, 1);
	device->CreateShaderResourceView(cursorInfo.texture.get(), &srvDesc,
		descriptorHeap.GetCpuHandle(cursorInfo.textureSrvOffset));

	return S_OK;
}

void CursorDrawer::_ClearCursorInfos() noexcept {
	_lastRawCursorHandle = NULL;
	_hCurCursor = NULL;
	_curCursorInfo = nullptr;

	auto& descriptorHeap = _d3d12Context->GetDescriptorHeap();
	auto& rtvDescriptorHeap = _d3d12Context->GetDescriptorHeap(true);

	for (const auto& pair : _cursorInfos) {
		if (pair.second.textureSrvOffset != std::numeric_limits<uint32_t>::max()) {
			descriptorHeap.Free(pair.second.textureSrvOffset, 1);
		}
		if (pair.second.textureRtvOffset != std::numeric_limits<uint32_t>::max()) {
			rtvDescriptorHeap.Free(pair.second.textureRtvOffset, 1);
		}
		if (pair.second.originTextureSrvOffset != std::numeric_limits<uint32_t>::max()) {
			descriptorHeap.Free(pair.second.originTextureSrvOffset, 1);
		}
	}

	_cursorInfos.clear();
}

HRESULT CursorDrawer::_CreateColorPSO(
	bool isSrgb,
	winrt::com_ptr<ID3D12PipelineState>& result
) noexcept {
	if (!_colorRootSignature) {
		winrt::com_ptr<ID3DBlob> signature;
		{
			CD3DX12_DESCRIPTOR_RANGE1 srvRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0,
				D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE);
			D3D12_ROOT_PARAMETER1 rootParams[] = {
				{
					.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
					.Constants = {
						.Num32BitValues = 4
					},
					.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX
				},
				{
					.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
					.DescriptorTable = {
						.NumDescriptorRanges = 1,
						.pDescriptorRanges = &srvRange
					},
					.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL
				}
			};
			D3D12_STATIC_SAMPLER_DESC samplerDesc = {
				.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT,
				.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
				.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
				.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
				.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER,
				.ShaderRegister = 0
			};
			CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc(
				(UINT)std::size(rootParams), rootParams, 1, &samplerDesc, D3D12_ROOT_SIGNATURE_FLAG_NONE);

			HRESULT hr = D3DX12SerializeVersionedRootSignature(
				&rootSignatureDesc, _d3d12Context->GetRootSignatureVersion(), signature.put(), nullptr);
			if (FAILED(hr)) {
				Logger::Get().ComError("D3DX12SerializeVersionedRootSignature 失败", hr);
				return hr;
			}
		}

		HRESULT hr = _d3d12Context->GetDevice()->CreateRootSignature(
			0,
			signature->GetBufferPointer(),
			signature->GetBufferSize(),
			IID_PPV_ARGS(&_colorRootSignature)
		);
		if (FAILED(hr)) {
			Logger::Get().ComError("CreateRootSignature 失败", hr);
			return hr;
		}
	}

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {
		.pRootSignature = _colorRootSignature.get(),
		.VS = CD3DX12_SHADER_BYTECODE(CursorVS, sizeof(CursorVS)),
		.PS = CD3DX12_SHADER_BYTECODE(TextureBlitPS, sizeof(TextureBlitPS)),
		.BlendState = {
			.RenderTarget = {{
				// FinalColor = CursorColor.rgb + ScreenColor * CursorColor.a
				.BlendEnable = TRUE,
				.SrcBlend = D3D12_BLEND_ONE,
				.DestBlend = D3D12_BLEND_SRC_ALPHA,
				.BlendOp = D3D12_BLEND_OP_ADD,
				.SrcBlendAlpha = D3D12_BLEND_ONE,
				.DestBlendAlpha = D3D12_BLEND_ZERO,
				.BlendOpAlpha = D3D12_BLEND_OP_ADD,
				.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL
			}}
		},
		.SampleMask = UINT_MAX,
		.RasterizerState = {
			.FillMode = D3D12_FILL_MODE_SOLID,
			.CullMode = D3D12_CULL_MODE_NONE
		},
		.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE,
		.NumRenderTargets = 1,
		.RTVFormats = { isSrgb ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT },
		.SampleDesc = { .Count = 1 }
	};
	HRESULT hr = _d3d12Context->GetDevice()->CreateGraphicsPipelineState(
		&psoDesc, IID_PPV_ARGS(&result));
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateGraphicsPipelineState 失败", hr);
		return hr;
	}

	return S_OK;
}

HRESULT CursorDrawer::_CreateMaskPSO(
	bool isMonochrome,
	bool isSrgb,
	winrt::com_ptr<ID3D12PipelineState>& result
) noexcept {
	if (!_maskRootSignature) {
		winrt::com_ptr<ID3DBlob> signature;
		{
			CD3DX12_DESCRIPTOR_RANGE1 srvRange1(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0,
				D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE);
			CD3DX12_DESCRIPTOR_RANGE1 srvRange2(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0,
				D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE);
			D3D12_ROOT_PARAMETER1 rootParams[] = {
				{
					.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
					.Constants = {
						.ShaderRegister = 0,
						.Num32BitValues = 4
					},
					.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX
				},
				{
					.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
					.Constants = {
						.ShaderRegister = 1,
						.Num32BitValues = 7
					},
					.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL
				},
				{
					.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
					.DescriptorTable = {
						.NumDescriptorRanges = 1,
						.pDescriptorRanges = &srvRange1
					},
					.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL
				},
				{
					.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
					.DescriptorTable = {
						.NumDescriptorRanges = 1,
						.pDescriptorRanges = &srvRange2
					},
					.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL
				}
			};
			CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc(
				(UINT)std::size(rootParams), rootParams, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);

			HRESULT hr = D3DX12SerializeVersionedRootSignature(
				&rootSignatureDesc,
				_d3d12Context->GetRootSignatureVersion(),
				signature.put(),
				nullptr
			);
			if (FAILED(hr)) {
				Logger::Get().ComError("D3DX12SerializeVersionedRootSignature 失败", hr);
				return hr;
			}
		}

		HRESULT hr = _d3d12Context->GetDevice()->CreateRootSignature(
			0,
			signature->GetBufferPointer(),
			signature->GetBufferSize(),
			IID_PPV_ARGS(&_maskRootSignature)
		);
		if (FAILED(hr)) {
			Logger::Get().ComError("CreateRootSignature 失败", hr);
			return hr;
		}
	}

	D3D12_SHADER_BYTECODE psByteCode;
	if (isMonochrome) {
		if (isSrgb) {
			psByteCode = CD3DX12_SHADER_BYTECODE(
				MonochromeCursorPS_sRGB, sizeof(MonochromeCursorPS_sRGB));
		} else {
			psByteCode = CD3DX12_SHADER_BYTECODE(
				MonochromeCursorPS, sizeof(MonochromeCursorPS));
		}
	} else {
		if (isSrgb) {
			psByteCode = CD3DX12_SHADER_BYTECODE(
				MaskedCursorPS_sRGB, sizeof(MaskedCursorPS_sRGB));
		} else {
			psByteCode = CD3DX12_SHADER_BYTECODE(
				MaskedCursorPS, sizeof(MaskedCursorPS));
		}
	}

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {
		.pRootSignature = _maskRootSignature.get(),
		.VS = CD3DX12_SHADER_BYTECODE(CursorVS, sizeof(CursorVS)),
		.PS = psByteCode,
		.BlendState = {
			.RenderTarget = {{ .RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL }}
		},
		.SampleMask = UINT_MAX,
		.RasterizerState = {
			.FillMode = D3D12_FILL_MODE_SOLID,
			.CullMode = D3D12_CULL_MODE_NONE
		},
		.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE,
		.NumRenderTargets = 1,
		.RTVFormats = { isSrgb ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT },
		.SampleDesc = { .Count = 1 }
	};
	HRESULT hr = _d3d12Context->GetDevice()->CreateGraphicsPipelineState(
		&psoDesc, IID_PPV_ARGS(&result));
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateGraphicsPipelineState 失败", hr);
		return hr;
	}

	return S_OK;
}

HRESULT CursorDrawer::_CreateCursorResizerPSO() noexcept {
	winrt::com_ptr<ID3DBlob> signature;
	{
		CD3DX12_DESCRIPTOR_RANGE1 srvRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0,
			D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE);
		D3D12_ROOT_PARAMETER1 rootParams[] = {
			{
				.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
				.Constants = {
					.Num32BitValues = 4
				},
				.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL
			},
			{
				.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
				.DescriptorTable = {
					.NumDescriptorRanges = 1,
					.pDescriptorRanges = &srvRange
				},
				.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL
			}
		};
		D3D12_STATIC_SAMPLER_DESC samplerDesc = {
			.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR,
			.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
			.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
			.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
			.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER,
			.ShaderRegister = 0
		};
		CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc(
			(UINT)std::size(rootParams), rootParams, 1, &samplerDesc, D3D12_ROOT_SIGNATURE_FLAG_NONE);

		HRESULT hr = D3DX12SerializeVersionedRootSignature(
			&rootSignatureDesc,
			_d3d12Context->GetRootSignatureVersion(),
			signature.put(),
			nullptr
		);
		if (FAILED(hr)) {
			Logger::Get().ComError("D3DX12SerializeVersionedRootSignature 失败", hr);
			return hr;
		}
	}

	HRESULT hr = _d3d12Context->GetDevice()->CreateRootSignature(
		0,
		signature->GetBufferPointer(),
		signature->GetBufferSize(),
		IID_PPV_ARGS(&_cursorResizerRootSignature)
	);
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateRootSignature 失败", hr);
		return hr;
	}

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {
		.pRootSignature = _cursorResizerRootSignature.get(),
		.VS = CD3DX12_SHADER_BYTECODE(FullscreenVS, sizeof(FullscreenVS)),
		.PS = CD3DX12_SHADER_BYTECODE(CursorResizerPS, sizeof(CursorResizerPS)),
		.BlendState = {
			.RenderTarget = {{ .RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL }}
		},
		.SampleMask = UINT_MAX,
		.RasterizerState = {
			.FillMode = D3D12_FILL_MODE_SOLID,
			.CullMode = D3D12_CULL_MODE_NONE
		},
		.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE,
		.NumRenderTargets = 1,
		.RTVFormats = { DXGI_FORMAT_R16G16B16A16_FLOAT },
		.SampleDesc = { .Count = 1 }
	};
	hr = _d3d12Context->GetDevice()->CreateGraphicsPipelineState(
		&psoDesc, IID_PPV_ARGS(&_cursorResizerPSO));
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateGraphicsPipelineState 失败", hr);
		return hr;
	}

	return S_OK;
}

void CursorDrawer::_ClearRetiredResources(uint64_t completedFenceValue) noexcept {
	if (_retiredTempOriginTextures.empty()) {
		return;
	}

	// _retiredTempOriginTextures 中元素的 fenceValue 按升序排列
	auto it = std::find_if(
		_retiredTempOriginTextures.begin(),
		_retiredTempOriginTextures.end(),
		[&](const _RetiredTempOriginTexture& rt) {
			return rt.fenceValue > completedFenceValue;
		}
	);
	if (it == _retiredTempOriginTextures.begin()) {
		return;
	}

	auto& descriptorHeap = _d3d12Context->GetDescriptorHeap();
	for (auto it1 = _retiredTempOriginTextures.begin(); it1 != it; ++it1) {
		if (it1->srvOffset != std::numeric_limits<uint32_t>::max()) {
			descriptorHeap.Free(it1->srvOffset, 1);
		}
	}

	_retiredTempOriginTextures.erase(_retiredTempOriginTextures.begin(), it);
}

}

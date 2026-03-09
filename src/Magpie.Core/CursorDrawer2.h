#pragma once
#include <wil/registry.h>
#include <parallel_hashmap/phmap.h>
#include "ByteBuffer.h"

namespace Magpie {

class GraphicsContext;

class CursorDrawer2 {
public:
	CursorDrawer2() noexcept = default;
	CursorDrawer2(const CursorDrawer2&) = delete;
	CursorDrawer2(CursorDrawer2&&) = delete;

	~CursorDrawer2() noexcept;

	bool Initialize(
		GraphicsContext& graphicsContext,
		const RECT& srcRect,
		const RECT& rendererRect,
		const RECT& destRect,
		const ColorInfo& colorInfo
	) noexcept;

	bool CheckForRedraw(HCURSOR hCursor, POINT cursorPos) noexcept;

	HRESULT Draw(uint32_t curFrameSrvOffset) noexcept;

	void OnCursorVirtualizationStarted() noexcept {
		_isCursorVirtualized = true;
	}

	void OnCursorVirtualizationEnded() noexcept {
		_isCursorVirtualized = false;
	}

	void OnMoveStarted() noexcept {
		_isMoving = true;
	}

	void OnMoveEnded() noexcept {
		_isMoving = false;
	}

	void OnSrcMoveStarted() noexcept {
		_isSrcMoving = true;
	}

	void OnSrcMoveEnded() noexcept {
		_isSrcMoving = false;
	}

	void OnMoved(const RECT& rendererRect, const RECT& destRect) noexcept;

	void OnResized(const RECT& rendererRect, const RECT& destRect) noexcept;

	void OnColorInfoChanged(const ColorInfo& colorInfo) noexcept;

private:
	// SDR 色域下使用 sRGB 空间，否则使用线性 RGB 空间。截至 Win11 25H2，Windows 在 WCG
	// 和 HDR 下光标的色域和透明度经常变化，没有统一标准。
	enum class _CursorType {
		// 彩色光标
		// 纹理格式: DXGI_FORMAT_R16G16B16A16_FLOAT
		// 计算公式: FinalColor = CursorColor.rgb + ScreenColor * CursorColor.a
		// 纹理中 RGB 通道已预乘 A 通道 (premultiplied alpha)，A 通道已预先取反，这是为了
		// 减少着色器的计算量以及确保 (可能进行的) 双线性插值的准确性。
		Color = 0,
		// 单色光标
		// 纹理格式: DXGI_FORMAT_R8_UINT
		// 高四位为 AND 掩码，低四位为 XOR 掩码，值只能是 0 或 0xf。
		Monochrome,
		// 彩色掩码光标
		// 纹理格式: DXGI_FORMAT_R8G8B8A8_UNORM
		// A 通道只能是 0 或 255。为 0 时用 RGB 通道取代屏幕颜色，为 255 时将 RGB 通道和
		// 屏幕颜色进行异或操作。
		MaskedColor
	};

	struct _CursorInfo {
		_CursorType type;
		Size size;
		Point hotspot;
		winrt::com_ptr<ID3D12Resource> texture;
		uint32_t textureSrvOffset = std::numeric_limits<uint32_t>::max();

		Size originSize;
		ByteBuffer originTextureData;
		winrt::com_ptr<ID3D12Resource> originUploadBuffer;
		winrt::com_ptr<ID3D12Resource> originTexture;
	};

	_CursorInfo* _ResolveCursor(HCURSOR hCursor, POINT cursorPos, bool isAni) noexcept;

	Size _CalcCursorSize(
		Size cursorBmpSize,
		uint32_t cursorDpi,
		uint32_t monitorDpi
	) const noexcept;

	wil::unique_hcursor _TryResolveCursorResource(
		const ICONINFOEX& iconInfoEx,
		uint32_t preferedWidth
	) const noexcept;

	wil::unique_hcursor _TryResolveStandardCursor(
		const wchar_t* regValueName,
		int resId,
		uint32_t preferedWidth
	) const noexcept;

	bool _ResolveCursorPixels(_CursorInfo& cursorInfo, HBITMAP hColorBmp, HBITMAP hMaskBmp) const noexcept;

	HRESULT _InitializeCursorTexture(_CursorInfo& cursorInfo) noexcept;

	// 只能在同步 GPU 后调用
	void _ClearCursorInfos() noexcept;

	HRESULT _CreateColorPSO() noexcept;

	HRESULT _CreateMaskPSO(bool isMonochrome) noexcept;

	GraphicsContext* _graphicsContext = nullptr;
	Size _srcSize{};
	RECT _rendererRect{};
	RECT _destRect{};
	ColorInfo _colorInfo;

	// (HCURSOR, DPI) -> _CursorInfo
	// DPI 为 0 表示此光标不随 DPI 缩放
	phmap::flat_hash_map<std::pair<HCURSOR, uint32_t>, _CursorInfo> _cursorInfos;

	// 这两个成员用于检查自动隐藏光标
	HCURSOR _lastRawCursorHandle = NULL;
	std::chrono::steady_clock::time_point _lastCursorActiveTime;
	// 上次绘制的光标形状和位置
	HCURSOR _hCurCursor = NULL;
	POINT _curCursorPos{ std::numeric_limits<LONG>::max(), std::numeric_limits<LONG>::max() };
	_CursorInfo* _curCursorInfo = nullptr;

	// 监控“指针大小”选项变化
	wil::unique_registry_watcher_nothrow _regWatcher;
	DWORD _cursorBaseSize = 32;

	winrt::com_ptr<ID3D12RootSignature> _colorRootSignature;
	winrt::com_ptr<ID3D12PipelineState> _colorPSO;
	winrt::com_ptr<ID3D12RootSignature> _maskRootSignature;
	winrt::com_ptr<ID3D12PipelineState> _monochromePSO;
	winrt::com_ptr<ID3D12PipelineState> _maskedColorPSO;

	bool _isCursorVisible = true;
	bool _isMoving = false;
	bool _isCursorVirtualized = false;
	bool _isSrcMoving = false;
};

}

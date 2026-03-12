#pragma once
#include "ByteBuffer.h"
#include "SmallVector.h"
#include <parallel_hashmap/phmap.h>
#include <wil/registry.h>

namespace Magpie {

class D3D12Context;
class GraphicsContext;

class CursorDrawer {
public:
	CursorDrawer() noexcept = default;
	CursorDrawer(const CursorDrawer&) = delete;
	CursorDrawer(CursorDrawer&&) = delete;

	~CursorDrawer() noexcept;

	bool Initialize(
		D3D12Context& d3d12Context,
		const RECT& srcRect,
		const RECT& rendererRect,
		const RECT& destRect,
		const ColorInfo& colorInfo
	) noexcept;

	bool CheckForRedraw(HCURSOR hCursor, POINT cursorPos) noexcept;

	// backBuffer 不为空表示掩码光标在叠加层上
	HRESULT Draw(
		GraphicsContext& graphicsContext,
		uint64_t completedFenceValue,
		uint64_t nextFenceValue,
		uint32_t curFrameSrvOffset,
		ID3D12Resource* backBuffer = nullptr
	) noexcept;

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
		
		Size originSize;
		ByteBuffer originTextureData;
		winrt::com_ptr<ID3D12Resource> originUploadBuffer;
		winrt::com_ptr<ID3D12Resource> originTexture;

		uint32_t textureSrvOffset = std::numeric_limits<uint32_t>::max();
		uint32_t textureRtvOffset = std::numeric_limits<uint32_t>::max();
		uint32_t originTextureSrvOffset = std::numeric_limits<uint32_t>::max();
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

	HRESULT _InitializeCursorTexture(
		GraphicsContext& graphicsContext,
		_CursorInfo& cursorInfo
	) noexcept;

	// 只能在同步 GPU 后调用
	void _ClearCursorInfos() noexcept;

	HRESULT _CreateColorPSO(bool isSrgb, winrt::com_ptr<ID3D12PipelineState>& result) noexcept;

	HRESULT _CreateMaskPSO(
		bool isMonochrome,
		bool isSrgb,
		winrt::com_ptr<ID3D12PipelineState>& result
	) noexcept;

	HRESULT _CreateCursorResizerPSO() noexcept;

	void _ClearRetiredResources(uint64_t completedFenceValue) noexcept;

	D3D12Context* _d3d12Context = nullptr;
	Size _srcSize{};
	RECT _rendererRect{};
	RECT _destRect{};
	ColorInfo _colorInfo;

	// 监控“指针大小”选项变化
	wil::unique_registry_watcher_nothrow _regWatcher;
	DWORD _cursorBaseSize = 32;

	// (HCURSOR, DPI) -> _CursorInfo
	// DPI 为 0 表示此光标不随 DPI 缩放
	phmap::flat_hash_map<std::pair<HCURSOR, uint32_t>, _CursorInfo> _cursorInfos;

	// 保存解析失败的光标以避免重复尝试
	phmap::flat_hash_set<HCURSOR> _unresolvableCursors;

	// 这两个成员用于检查自动隐藏光标
	HCURSOR _lastRawCursorHandle = NULL;
	std::chrono::steady_clock::time_point _lastCursorActiveTime;
	// 上次绘制的光标形状和位置
	HCURSOR _hCurCursor = NULL;
	POINT _curCursorPos{ std::numeric_limits<LONG>::max(), std::numeric_limits<LONG>::max() };
	_CursorInfo* _curCursorInfo = nullptr;

	// 用于从渲染目标复制光标下区域
	winrt::com_ptr<ID3D12Resource> _tempOriginTexture;
	Size _tempOriginTextureSize{};
	uint32_t _tempOriginTextureSrvOffset = std::numeric_limits<uint32_t>::max();
	
	struct _RetiredTempOriginTexture {
		winrt::com_ptr<ID3D12Resource> texture;
		uint64_t fenceValue;
		uint32_t srvOffset;
	};
	SmallVector<_RetiredTempOriginTexture, 1> _retiredTempOriginTextures;

	winrt::com_ptr<ID3D12RootSignature> _colorRootSignature;
	winrt::com_ptr<ID3D12PipelineState> _colorPSO;
	winrt::com_ptr<ID3D12PipelineState> _colorSrgbPSO;
	winrt::com_ptr<ID3D12RootSignature> _maskRootSignature;
	winrt::com_ptr<ID3D12PipelineState> _monochromePSO;
	winrt::com_ptr<ID3D12PipelineState> _monochromeSrgbPSO;
	winrt::com_ptr<ID3D12PipelineState> _maskedColorPSO;
	winrt::com_ptr<ID3D12PipelineState> _maskedColorSrgbPSO;
	winrt::com_ptr<ID3D12RootSignature> _cursorResizerRootSignature;
	winrt::com_ptr<ID3D12PipelineState> _cursorResizerPSO;

	bool _isCursorVisible = true;
	bool _isMoving = false;
	bool _isCursorVirtualized = false;
	bool _isSrcMoving = false;
};

}

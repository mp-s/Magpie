// Copyright (c) Xu
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.


#include "pch.h"
#include "App.h"
#include "CommonSharedConstants.h"
#include "Logger.h"
#include "TouchHelper.h"
#include "Win32Helper.h"
#ifdef _DEBUG
#include <d3d12sdklayers.h>
#include <dxgidebug.h>
#endif
#include <dxgi1_6.h>

#ifdef _DEBUG

// Debug 配置下使用 Agility SDK 辅助调试
extern "C" { __declspec(dllexport) extern const UINT D3D12SDKVersion = 618; }
extern "C" { __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\"; }

// 用于辅助开发和调试
static Ignore _ = [] {
	DEBUG_INFO.enableGPUBasedValidation = false;
	DEBUG_INFO.gpuSlowDownFactor = 0.0f;
	return Ignore();
}();
#endif

using namespace Magpie;
using namespace winrt::Magpie::implementation;

// 将当前目录设为程序所在目录
static void SetWorkingDir() noexcept {
	FAIL_FAST_IF_WIN32_BOOL_FALSE(SetCurrentDirectory(
		Win32Helper::GetExePath().parent_path().c_str()));
}

static void InitializeLogger(const wchar_t* logFilePath) noexcept {
	// 最多两个日志文件，每个最多 500KB
	Logger::Get().Initialize(
		spdlog::level::info,
		logFilePath,
		CommonSharedConstants::LOG_MAX_SIZE,
		1
	);
}

static void InitializeDirectX() noexcept {
#ifdef _DEBUG
	winrt::com_ptr<IDXGIInfoQueue> dxgiInfoQueue;
	if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&dxgiInfoQueue)))) {
		// 发生错误时中断
		dxgiInfoQueue->SetBreakOnSeverity(DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_CORRUPTION, true);
		dxgiInfoQueue->SetBreakOnSeverity(DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_ERROR, true);
	}

	{
		winrt::com_ptr<ID3D12Debug1> debugController;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
			debugController->EnableDebugLayer();
			
			if (DEBUG_INFO.enableGPUBasedValidation) {
				// 会产生警告消息，而且有严重内存泄露
				debugController->SetEnableGPUBasedValidation(TRUE);
			}

			// Win11 开始支持生成默认名字，包含资源的基本属性
			if (winrt::com_ptr<ID3D12Debug5> debugController5 = debugController.try_as<ID3D12Debug5>()) {
				debugController5->SetEnableAutoName(TRUE);
			}
		}
	}
#endif

	// 声明支持 TDR 恢复
	DXGIDeclareAdapterRemovalSupport();
}

int APIENTRY wWinMain(
	_In_ HINSTANCE /*hInstance*/,
	_In_opt_ HINSTANCE /*hPrevInstance*/,
	_In_ wchar_t* lpCmdLine,
	_In_ int /*nCmdShow*/
) {
#ifdef _DEBUG
	SetThreadDescription(GetCurrentThread(), L"Magpie-主线程");
#endif
	
	// 堆损坏时终止进程
	HeapSetInformation(NULL, HeapEnableTerminationOnCorruption, nullptr, 0);

	SetWorkingDir();

	enum {
		Normal,
		RegisterTouchHelper,
		UnRegisterTouchHelper
	} mode = [&]() {
		if (lpCmdLine == L"-r"sv) {
			return RegisterTouchHelper;
		} else if (lpCmdLine == L"-ur"sv) {
			return UnRegisterTouchHelper;
		} else {
			return Normal;
		}
	}();

	InitializeLogger(mode == Normal ?
		CommonSharedConstants::LOG_PATH :
		CommonSharedConstants::REGISTER_TOUCH_HELPER_LOG_PATH);

	Logger::Get().Info(fmt::format("程序启动\n\t版本: {}\n\tOS 版本: {}\n\t管理员: {}",
#ifdef MP_VERSION_STRING
		STRINGIFY(MP_VERSION_STRING),
#elif defined(MP_COMMIT_ID)
		"dev (" STRINGIFY(MP_COMMIT_ID) ")",
#else
		"dev",
#endif
		Win32Helper::GetOSVersion().ToString<char>(),
		Win32Helper::IsProcessElevated() ? "是" : "否"
	));

	if (mode == RegisterTouchHelper) {
		// 使 TouchHelper 获得 UIAccess 权限
		return Magpie::TouchHelper::Register() ? 0 : 1;
	} else if (mode == UnRegisterTouchHelper) {
		return Magpie::TouchHelper::Unregister() ? 0 : 1;
	}

	// 程序结束时也不应调用 uninit_apartment
	// 见 https://kennykerr.ca/2018/03/24/cppwinrt-hosting-the-windows-runtime/
	winrt::init_apartment(winrt::apartment_type::single_threaded);

	InitializeDirectX();

	auto& app = App::Get();
	if (!app.Initialize(lpCmdLine)) {
		return 0;
	}

	return app.Run();
}

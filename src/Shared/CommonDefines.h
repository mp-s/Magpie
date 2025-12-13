#pragma once
#include "StrMacros.h"

using namespace std::string_literals;
using namespace std::string_view_literals;
using namespace std::chrono_literals;

#ifdef WINRT_IMPL_COROUTINES
// 导入 winrt 命名空间的 co_await 重载
// https://devblogs.microsoft.com/oldnewthing/20191219-00/?p=103230
using winrt::operator co_await;
#endif

#define DEFINE_FLAG_ACCESSOR(Name, FlagBit, FlagsVar) \
	bool Name() const noexcept { return WI_IsFlagSet(FlagsVar, FlagBit); } \
	void Name(bool value) noexcept { WI_UpdateFlag(FlagsVar, FlagBit, value); }

#define SWP_NO_ACTIVATE_MOVE_SIZE (SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE)

struct Ignore {
	constexpr Ignore() noexcept = default;

	template <typename T>
	constexpr Ignore(const T&) noexcept {}

	template <typename T>
	constexpr const Ignore& operator=(const T&) const noexcept {
		return *this;
	}
};

template <typename T>
static constexpr inline T FLOAT_EPSILON = std::numeric_limits<T>::epsilon() * 100;

// 不支持 nan 和无穷大
template <typename T>
static bool IsApprox(T l, T r) noexcept {
	static_assert(std::is_floating_point_v<T>, "T 必须是浮点数类型");
	return std::abs(l - r) < FLOAT_EPSILON<T>;
}

// 单位为微秒
template <typename Fn>
static uint32_t Measure(const Fn& func) noexcept {
	using namespace std::chrono;

	auto t = steady_clock::now();
	func();
	auto dura = duration_cast<microseconds>(steady_clock::now() - t);

	return (uint32_t)dura.count();
}

#ifdef MP_DEBUG_INFO
// 不要定义成匿名类，和 inline 冲突
struct DebugInfo {
	// 启用 GPU-based validation
	bool enableGPUBasedValidation = false;
	// 模拟低速 GPU
	float gpuSlowDownFactor = 0.0f;

	// 用于同步对下面成员的访问
	wil::srwlock lock;

	// 生产者当前帧序号
	uint32_t producerFrameNumber = 0;
	// 消费者者当前帧序号
	uint32_t consumerFrameNumber = 0;
	// 消费者落后生产者的帧数
	uint32_t consumerLatency = 0;

	// 这几个成员用于计算捕获帧被 DWM 呈现到被 Magpie 呈现的延迟

	// 捕获的帧被 DWM 呈现的时间
	int64_t dtmCaptureQPC = 0;
	// 追踪的帧对应的帧序号
	uint32_t dtmFrameNumer = 0;
	// 此帧被呈现前的交换链 VSync 计数，用于判断是否已被呈现
	uint32_t dtmSwapChainRefreshCount = 0;
	// 单位为微秒。允许小于 0，即 Magpie 比 DWM 更早呈现
	int32_t dwmToMagpieLatency = 0;
};
inline DebugInfo DEBUG_INFO;
#endif

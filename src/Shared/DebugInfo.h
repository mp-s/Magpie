#pragma once
#include "CommonDefines.h"

#ifdef MP_DEBUG_INFO

// 不要定义成匿名类，和 inline 冲突
struct DebugInfo {
	// 模拟低速 GPU
	float gpuSlowDownFactor = 0.0f;
	// 启用 GPU-based validation
	bool enableGPUBasedValidation = false;
	// 禁用 GPU 的动态时钟频率调整
	bool enableStablePower = false;

	// 用于同步对下面成员的访问
	wil::srwlock lock;

	// 生产者当前帧序号
	uint32_t producerFrameNumber = 1;
	// 消费者者当前帧序号
	uint32_t consumerFrameNumber = 0;
	// 消费者落后生产者的帧数
	uint32_t consumerLatency = 0;

	// 下面几个成员用于测量捕获帧被 DWM 呈现到被 Magpie 呈现的延迟

	// 捕获的帧被 DWM 呈现的时间
	int64_t dtmDwmQPC = 0;
	// 追踪的帧的帧序号
	uint32_t dtmFrameNumer = 0;
	// 此帧被呈现前的交换链 VSync 计数，用于判断是否已被呈现
	uint32_t dtmSwapChainRefreshCount = 0;
	// 单位为微秒。允许小于 0，即 Magpie 比 DWM 更早呈现
	int32_t dwmToMagpieLatency = 0;

	// 下面几个成员用于测量从捕获到呈现的耗时

	// 捕获帧到达时间
	int64_t ctpCaptureQPC = 0;
	// 捕获帧
	void* ctpCapturedFrame = nullptr;
	// 追踪的帧的帧序号
	uint32_t ctpFrameNumer = 0;
	// 单位为微秒
	uint32_t captureToPresentLatency = 0;
};

inline DebugInfo DEBUG_INFO;

#endif

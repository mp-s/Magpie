uint __Bfe(uint src, uint off, uint bits) {
    uint mask = (1u << bits) - 1;
    return (src >> off) & mask;
}

uint __BfiM(uint src, uint ins, uint bits) {
    uint mask = (1u << bits) - 1;
    return (ins & mask) | (src & (~mask));
}

// 来自 https://github.com/GPUOpen-Effects/FidelityFX-FSR/blob/a21ffb8f6c13233ba336352bdff293894c706575/ffx-fsr/ffx_a.h#L2304
uint2 Rmp8x8(uint a) {
    return uint2(__Bfe(a, 1u, 3u), __BfiM(__Bfe(a, 3u, 3u), a, 1u));
}

// 来自 https://github.com/GPUOpen-Effects/FidelityFX-FSR/blob/a21ffb8f6c13233ba336352bdff293894c706575/ffx-fsr/ffx_a.h#L2189-L2190
float3 LinearToSrgb(float3 c) {
    float3 j = { 0.0031308 * 12.92, 12.92, 1.0 / 2.4 };
    float2 k = { 1.055, -0.055 };
    return clamp(j.xxx, c * j.yyy, pow(c, j.zzz) * k.xxx + k.yyy);
}

// 接受线性 RGB 输入
float Luminance(float3 color) {
    // 参数来自 https://en.wikipedia.org/wiki/Relative_luminance
	return dot(color, float3(0.2126, 0.7152, 0.0722));
}

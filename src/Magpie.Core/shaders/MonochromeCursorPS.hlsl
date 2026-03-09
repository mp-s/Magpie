#include "Common.hlsli"

cbuffer RootConstants : register(b1) {
	uint2 cursorTexSize;
	uint2 cursorSize;
	uint2 originOffset;
	float sdrWhiteLevel;
	uint shouldEncodeSrgb;
};

Texture2D<uint> cursorTex : register(t0);
Texture2D<float4> originTex : register(t1);

float4 main(noperspective float2 uv : TEXCOORD) : SV_TARGET {
	uint2 coord = uint2(uv * cursorTexSize);
	
	// 高四位是 AND 掩码， 低四位是 XOR 掩码
	uint mask = cursorTex[coord];
	uint andMask = mask & 0xF0u;
	uint xorMask = mask & 0x0Fu;
	
	if (!andMask) {
		// 黑色或白色
		float c = xorMask ? sdrWhiteLevel : 0.0f;
		return float4(c, c, c, 1.0f);
	}
	
	float3 origin = originTex[uv * cursorSize + originOffset].rgb;
	
	[branch]
	if (shouldEncodeSrgb) {
		origin = EncodeSrgb(saturate(origin));
	}
	
	if (xorMask) {
		// 反色
		float white = max(max(max(origin.r, origin.g), origin.b), sdrWhiteLevel);
		origin = white - origin;
	}
	
	return float4(origin, 1.0f);
}

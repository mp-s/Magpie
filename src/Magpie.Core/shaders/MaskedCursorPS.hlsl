#include "Common.hlsli"

cbuffer RootConstants : register(b1) {
	uint2 cursorTexSize;
	uint2 cursorSize;
	uint2 originOffset;
#ifdef MP_SRGB
	uint shouldEncodeSrgb;
#else
	float sdrWhiteLevel;
#endif
};

Texture2D<float4> cursorTex : register(t0);
Texture2D<float4> originTex : register(t1);

float4 main(noperspective float2 uv : TEXCOORD) : SV_TARGET {
	uint2 coord = uint2(uv * cursorTexSize);
	
	// A 为 0 时用 RGB 通道取代屏幕颜色，为 1 时将 RGB 通道和屏幕颜色进行异或操作
	float4 mask = cursorTex[coord];
	
	if (mask.a < 0.5f) {
		return float4(mask.rgb, 1);
	}
	
	float3 origin = originTex[uv * cursorSize + originOffset].rgb;
	
#ifdef MP_SRGB
	[branch]
	if (shouldEncodeSrgb) {
		origin = EncodeSrgb(saturate(origin));
	}
#else
	float white = max(max(max(origin.r, origin.g), origin.b), sdrWhiteLevel);
	origin = saturate(origin / white);
#endif
	
	// 255.001953 来自
	// https://stackoverflow.com/questions/52103720/why-does-d3dcolortoubyte4-multiplies-components-by-255-001953f
	origin = (uint3(origin * 255.001953f) ^ uint3(mask.rgb * 255.001953f)) / 255.0f;
	
#ifndef MP_SRGB
	origin *= white;
#endif
	
	return float4(origin, 1);
}

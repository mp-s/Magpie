#include "Common.hlsli"

Texture2D<float4> inputTex : register(t0);
RWTexture2D<float4> outputTex : register(u0);

float4 LoadTexel(uint2 gxy) {
	float4 color = inputTex[gxy];

#ifdef MP_SRGB
	color.rgb = LinearToSrgb(saturate(color.rgb));
#endif
	return color;
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_GroupThreadID, uint3 gid : SV_GroupID) {
	uint2 gxy = (gid.xy << uint2(4, 3)) + tid.xy;
	outputTex[gxy] = LoadTexel(gxy);
	
	gxy.x += 8u;
	outputTex[gxy] = LoadTexel(gxy);
	
	gxy.y += 8u;
	outputTex[gxy] = LoadTexel(gxy);
	
	gxy.x -= 8u;
	outputTex[gxy] = LoadTexel(gxy);
}

#include "Common.hlsli"

Texture2D<float4> inputTex : register(t0);
Texture2D<float4> originTex : register(t1);
RWTexture2D<float4> outputTex : register(u0);

float4 Post(uint2 gxy) {
	float3 inputColor = inputTex[gxy].rgb;
	float3 originColor = originTex[gxy].rgb;
	
	float originLuma = Luminance(originColor);
	
	if (originLuma < 1e-3f) {
		return float4(originColor, 1);
	} else {
		// 将 origin 的颜色缩放到 input 的亮度水平
		float inputLuma = max(Luminance(inputColor), 0.0f);
		float3 originScaled = originColor * inputLuma * rcp(originLuma);
		
		// 混合
		const float strength = 0.75f;
		float3 result = lerp(inputColor, originScaled, strength);
		return float4(result, 1);
	}
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_GroupThreadID, uint3 gid : SV_GroupID) {
	uint2 gxy = (gid.xy << 4) + tid.xy;
	outputTex[gxy] = Post(gxy);
	
	gxy.x += 8u;
	outputTex[gxy] = Post(gxy);
	
	gxy.y += 8u;
	outputTex[gxy] = Post(gxy);
	
	gxy.x -= 8u;
	outputTex[gxy] = Post(gxy);
}

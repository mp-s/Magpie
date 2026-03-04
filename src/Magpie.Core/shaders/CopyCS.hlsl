Texture2D<float4> inputTex : register(t0);
RWTexture2D<float4> outputTex : register(u0);

#ifdef MP_SRGB
// 来自 https://github.com/GPUOpen-Effects/FidelityFX-FSR/blob/a21ffb8f6c13233ba336352bdff293894c706575/ffx-fsr/ffx_a.h#L2189-L2190
float3 LinearToSrgb(float3 c) {
	float3 j = { 0.0031308 * 12.92, 12.92, 1.0 / 2.4 };
	float2 k = { 1.055, -0.055 };
	return clamp(j.xxx, c * j.yyy, pow(c, j.zzz) * k.xxx + k.yyy);
}
#endif

float4 LoadTexel(uint2 gxy) {
    float4 color = inputTex[gxy.xy];

#ifdef MP_SRGB
    color.rgb = LinearToSrgb(color.rgb);
#endif
    return color;
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_GroupThreadID, uint3 gid : SV_GroupID) {
    uint2 gxy = (gid.xy << uint2(4, 3)) + tid.xy;
	outputTex[gxy] = LoadTexel(gxy);
    
	gxy.x += 8u;
	outputTex[gxy] = LoadTexel(gxy);
}

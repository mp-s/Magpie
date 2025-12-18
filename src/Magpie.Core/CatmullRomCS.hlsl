// 移植自 https://github.com/obsproject/obs-studio/blob/master/libobs/data/bicubic_scale.effect

cbuffer RootConstants : register(b0) {
	uint2 inputSize;
	uint2 outputSize;
	float2 inputPt;
	float2 outputPt;
};

Texture2D<float4> input : register(t0);

RWTexture2D<unorm float4> output : register(u0);

SamplerState linearSampler : register(s0);

float4 weight4(float x) {
	// Sharper version.  May look better in some cases. B=0, C=0.75
	return float4(
		((-0.75 * x + 1.5) * x - 0.75) * x,
		(1.25 * x - 2.25) * x * x + 1.0,
		((-1.25 * x + 1.5) * x + 0.75) * x,
		(0.75 * x - 0.75) * x * x
	);
}

#ifdef MP_SRGB
// 来自 https://github.com/GPUOpen-Effects/FidelityFX-FSR/blob/a21ffb8f6c13233ba336352bdff293894c706575/ffx-fsr/ffx_a.h#L2189-L2190
float3 LinearToSrgb(float3 c) {
	float3 j = { 0.0031308 * 12.92, 12.92, 1.0 / 2.4 };
	float2 k = { 1.055, -0.055 };
	return clamp(j.xxx, c * j.yyy, pow(c, j.zzz) * k.xxx + k.yyy);
}
#endif

float4 CatmullRom(float2 pos) {
	pos *= inputSize;
	float2 pos1 = floor(pos - 0.5f) + 0.5f;
	float2 f = pos - pos1;

	float4 rowtaps = weight4(f.x);
	float4 coltaps = weight4(f.y);
	
	float2 uv1 = pos1 * inputPt;
	float2 uv0 = uv1 - inputPt;
	float2 uv2 = uv1 + inputPt;
	float2 uv3 = uv2 + inputPt;

	float u_weight_sum = rowtaps.y + rowtaps.z;
	float u_middle_offset = rowtaps.z * inputPt.x / u_weight_sum;
	float u_middle = uv1.x + u_middle_offset;

	float v_weight_sum = coltaps.y + coltaps.z;
	float v_middle_offset = coltaps.z * inputPt.y / v_weight_sum;
	float v_middle = uv1.y + v_middle_offset;

	int2 coord_top_left = int2(max(uv0 * inputSize, 0.5f));
	int2 coord_bottom_right = int2(min(uv3 * inputSize, inputSize - 0.5f));

	float3 top = input.Load(int3(coord_top_left, 0)).rgb * rowtaps.x;
	top += input.SampleLevel(linearSampler, float2(u_middle, uv0.y), 0).rgb * u_weight_sum;
	top += input.Load(int3(coord_bottom_right.x, coord_top_left.y, 0)).rgb * rowtaps.w;
	float3 total = top * coltaps.x;

	float3 middle = input.SampleLevel(linearSampler, float2(uv0.x, v_middle), 0).rgb * rowtaps.x;
	middle += input.SampleLevel(linearSampler, float2(u_middle, v_middle), 0).rgb * u_weight_sum;
	middle += input.SampleLevel(linearSampler, float2(uv3.x, v_middle), 0).rgb * rowtaps.w;
	total += middle * v_weight_sum;

	float3 bottom = input.Load(int3(coord_top_left.x, coord_bottom_right.y, 0)).rgb * rowtaps.x;
	bottom += input.SampleLevel(linearSampler, float2(u_middle, uv3.y), 0).rgb * u_weight_sum;
	bottom += input.Load(int3(coord_bottom_right, 0)).rgb * rowtaps.w;
	total += bottom * coltaps.w;

#ifdef MP_SRGB
	total = LinearToSrgb(saturate(total));
#endif
	return float4(total, 1);
}

uint Bfe(uint src, uint off, uint bits) {
	uint mask = (1u << bits) - 1;
	return (src >> off) & mask;
}

uint BfiM(uint src, uint ins, uint bits) {
	uint mask = (1u << bits) - 1;
	return (ins & mask) | (src & (~mask));
}

uint2 Rmp8x8(uint a) {
	return uint2(Bfe(a, 1u, 3u), BfiM(Bfe(a, 3u, 3u), a, 1u));
}

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_GroupThreadID, uint3 gid : SV_GroupID) {
	uint2 gxy = (gid.xy << uint2(4, 3)) + Rmp8x8(tid.x);
	float2 pos = (gxy + 0.5f) * outputPt;

	output[gxy] = CatmullRom(pos);

	gxy.x += 8u;
    pos.x += 8 * outputPt.x;
	output[gxy] = CatmullRom(pos);
}

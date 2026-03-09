#include "Common.hlsli"

cbuffer RootConstants : register(b1) {
	uint2 texSize;
	uint2 originOffset;
	float sdrWhiteLevel;
	uint isSdr;
	uint shouldEncodeSrgb;
};

Texture2D<uint> cursorTex : register(t0);
Texture2D<float4> originTex : register(t1);

float4 main(noperspective float2 uv : TEXCOORD) : SV_TARGET {
	uint2 coord = uint2(uv * texSize);
	
	// 高四位是 AND 掩码， 低四位是 XOR 掩码
	uint mask = cursorTex[coord];
    uint andMask = mask & 0xF0u;
    uint xorMask = mask & 0x0Fu;
	
    if (!andMask) {
        float v = xorMask ? sdrWhiteLevel : 0.0f;
        return float4(v, v, v, 1.0f);
    }
	
    float3 origin = originTex[coord + originOffset].rgb;
	
	if (isSdr) {
		if (shouldEncodeSrgb) {
			origin = EncodeSrgb(saturate(origin));
		}

		if (xorMask) {
			origin = 1.0f - origin;
		}
	} else {
		if (xorMask) {
			float white = max(max(origin.r, origin.g), max(origin.b, sdrWhiteLevel));
			origin = white - origin;
		}
	}
	
    return float4(origin, 1.0f);
}

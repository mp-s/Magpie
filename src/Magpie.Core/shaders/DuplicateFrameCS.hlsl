cbuffer RootConstants : register(b0) {
    float2 texPt;
    uint target;
    uint resultOffest;
    uint2 texOffset;
};

// 无需同步
RWStructuredBuffer<uint> result : register(u0);

Texture2D tex1 : register(t0);
Texture2D tex2 : register(t1);

SamplerState sam : register(s0);

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_GroupThreadID, uint3 gid : SV_GroupID) {
    if (result[resultOffest] == target) {
		return;
	}
	
    const int2 gxy = texOffset + (gid.xy << 4) + (tid.xy << 1);
	const float2 pos = (gxy + 1) * texPt;
	
	if (any(tex1.GatherRed(sam, pos) != tex2.GatherRed(sam, pos))) {
        result[resultOffest] = target;
		return;
	}
	
	if (any(tex1.GatherGreen(sam, pos) != tex2.GatherGreen(sam, pos))) {
        result[resultOffest] = target;
		return;
	}
	
	if (any(tex1.GatherBlue(sam, pos) != tex2.GatherBlue(sam, pos))) {
        result[resultOffest] = target;
    }
}

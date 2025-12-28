cbuffer RootConstants : register(b0) {
    float2 texPt;
    uint target;
    uint resultOffest;
    uint4 dirtyRect;
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
	
    const uint2 gxy = dirtyRect.xy + (gid.xy << 4) + (tid.xy << 1);
	
	// 基本越界检查。由于每个线程采样 2x2，仍有可能越界一个像素，但影响很小。最坏的情况是把
	// 不变的脏矩形错误识别为变化，增加一点复制开销，这个几率很小。
	if (gxy.x >= dirtyRect.z || gxy.y >= dirtyRect.w) {
		return;
	}
	
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

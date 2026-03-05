Texture2D<float4> inputTex : register(t0);
Texture2D<float4> originTex : register(t1);
RWTexture2D<float4> outputTex : register(u0);

float4 LoadTexel(uint2 gxy) {
    float3 color = inputTex[gxy].rgb;
	float3 origin = originTex[gxy].rgb;
    
    // 还原超出 sRGB 的颜色
	float sum = origin.r + origin.g + origin.b;
	if (sum > 1e-5) {
        // 归一化
		origin *= rcp(sum);
    
		float3 adjust = origin - saturate(origin);
		color += adjust * (color.r + color.g + color.b);
	}
    
    return float4(color, 1);
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_GroupThreadID, uint3 gid : SV_GroupID) {
    uint2 gxy = (gid.xy << 4) + tid.xy;
    outputTex[gxy] = LoadTexel(gxy);
	
    gxy.x += 8u;
    outputTex[gxy] = LoadTexel(gxy);
	
    gxy.y += 8u;
    outputTex[gxy] = LoadTexel(gxy);
	
    gxy.x -= 8u;
    outputTex[gxy] = LoadTexel(gxy);
}

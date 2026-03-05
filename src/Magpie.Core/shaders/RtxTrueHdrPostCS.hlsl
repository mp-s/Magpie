Texture2D<float4> inputTex : register(t0);
Texture2D<float4> originTex : register(t1);
RWTexture2D<float4> outputTex : register(u0);

float4 LoadTexel(uint2 gxy) {
    float3 color = inputTex[gxy].rgb;
    
    // 还原超出 sRGB 的颜色
    float3 origin = originTex[gxy].rgb;
    origin /= origin.r + origin.g + origin.b;
    
    float3 adjust = min(origin, 0) + max(origin - 1, 0);
    color += adjust * (color.r + color.g + color.b);
    
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

cbuffer RootConstants : register(b0) {
    float sdrWhiteLevel;
};

Texture2D<float4> inputTex : register(t0);
RWTexture2D<float4> outputTex : register(u0);

float4 LoadTexel(uint2 gxy) {
	// TrueHDR 只接受 linear sRGB 输入，这意味着必须执行色调映射。但考虑到 TrueHDR 
	// 使用 AI 算法，保留原始色调才能获得最佳效果，因此这里只简单截断（outputTex 为
	// unorm 格式，写入时将自动截断），然后在后处理中还原色调。
	// 
	// 不支持 HDR 输出，超出 sdrWhiteLevel 将被截断。
    return inputTex[gxy] / sdrWhiteLevel;
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

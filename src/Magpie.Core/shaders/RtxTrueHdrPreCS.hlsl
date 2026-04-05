cbuffer RootConstants : register(b0) {
    float sdrWhiteLevel;
};

Texture2D<float4> inputTex : register(t0);
RWTexture2D<unorm float4> outputTex : register(u0);

float4 LoadTexel(uint2 gxy) {
	// 文档没记录，但测试表明，TrueHDR 只接受 BT.2020 色域输入，若是输入 sRGB 会严重偏色。
	// 不支持 HDR 输入，超出 sdrWhiteLevel 将被截断。
	static const float3x3 mat709to2020 = float3x3(
        0.6274040f, 0.3292854f, 0.0433106f,
        0.0690973f, 0.9195410f, 0.0113617f,
        0.0163914f, 0.0880133f, 0.8955953f
    );
	
	// 超出 BT.2020 的颜色将被截断
	return float4(mul(mat709to2020, inputTex[gxy].rgb / sdrWhiteLevel), 1);
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

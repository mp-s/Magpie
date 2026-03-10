cbuffer RootConstants : register(b0) {
	uint2 inputSize;
	float2 inputPt;
	float2 outputPt;
};

Texture2D inputTex : register(t0);
SamplerState linearSampler : register(s0);

float4 main(noperspective float2 coord : TEXCOORD) : SV_Target {
	return inputTex.Sample(linearSampler, coord);
}

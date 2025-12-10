struct PSInput {
    noperspective float2 uv : TEXCOORD;
    noperspective float4 position : SV_POSITION;
};

PSInput main(float2 position : POSITION, float2 uv : TEXCOORD) {
    PSInput result;
    result.position = float4(position, 0, 1);
    result.uv = uv;
    return result;
}

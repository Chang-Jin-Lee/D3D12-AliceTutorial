cbuffer ShadowConstantBuffer : register(b0)
{
    matrix mvp; // object's world * light view * light projection
};

// Depth-only pass: the rasterizer's own depth write is all we need, so
// there is no pixel shader bound for this PSO at all.
float4 VSMain(float3 position : POSITION) : SV_POSITION
{
    return mul(float4(position, 1.0f), mvp);
}

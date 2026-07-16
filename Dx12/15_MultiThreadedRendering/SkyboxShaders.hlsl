cbuffer SkyboxConstantBuffer : register(b0)
{
    matrix mvp;
    float4 skyColor;     // top-of-sky color
    float4 horizonColor; // color at the horizon
};

struct PSInput
{
    float4 position : SV_POSITION;
    float3 localPosition : TEXCOORD;
};

// The skybox cube is centered on the world origin, so a vertex's local
// position IS the direction from the center outward - no separate
// direction vector needs to be computed.
PSInput VSMain(float3 position : POSITION)
{
    PSInput result;
    result.position = mul(float4(position, 1.0f), mvp);
    result.localPosition = position;
    return result;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float3 direction = normalize(input.localPosition);
    float t = saturate(direction.y * 0.5f + 0.5f);
    float3 color = lerp(horizonColor.rgb, skyColor.rgb, t);
    return float4(color, 1.0f);
}

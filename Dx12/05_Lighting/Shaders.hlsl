cbuffer SceneConstantBuffer : register(b0)
{
    matrix mvp;
    matrix world;
    float4 lightDirection; // world-space, normalized (points FROM the light)
    float4 lightColor;
    float4 ambientColor;
};

Texture2D g_texture : register(t0);
SamplerState g_sampler : register(s0);

struct PSInput
{
    float4 position : SV_POSITION;
    float3 worldNormal : NORMAL;
    float2 uv : TEXCOORD;
};

PSInput VSMain(float3 position : POSITION, float3 normal : NORMAL, float2 uv : TEXCOORD)
{
    PSInput result;
    result.position = mul(float4(position, 1.0f), mvp);
    result.worldNormal = mul(float4(normal, 0.0f), world).xyz;
    result.uv = uv;
    return result;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float3 normal = normalize(input.worldNormal);
    float diffuseTerm = max(dot(normal, -lightDirection.xyz), 0.0f);

    float4 texColor = g_texture.Sample(g_sampler, input.uv);
    float3 lit = texColor.rgb * (ambientColor.rgb + lightColor.rgb * diffuseTerm);
    return float4(lit, texColor.a);
}

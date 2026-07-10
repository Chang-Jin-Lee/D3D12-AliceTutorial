cbuffer SceneConstantBuffer : register(b0)
{
    matrix mvp;
    matrix world;
    float4 lightDirection; // world-space, normalized (points FROM the light)
    float4 lightColor;
    float4 ambientColor;
    float4 eyePosition;    // world-space camera position (.w unused)
    float4 specularColor;  // .rgb = highlight tint, .a = shininess (specular power)
};

Texture2D g_texture : register(t0);
SamplerState g_sampler : register(s0);

struct PSInput
{
    float4 position : SV_POSITION;
    float3 worldPosition : TEXCOORD1;
    float3 worldNormal : NORMAL;
    float2 uv : TEXCOORD0;
};

PSInput VSMain(float3 position : POSITION, float3 normal : NORMAL, float2 uv : TEXCOORD)
{
    PSInput result;
    result.position = mul(float4(position, 1.0f), mvp);
    result.worldPosition = mul(float4(position, 1.0f), world).xyz;
    result.worldNormal = mul(float4(normal, 0.0f), world).xyz;
    result.uv = uv;
    return result;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float3 normal = normalize(input.worldNormal);
    float3 toLight = -lightDirection.xyz;
    float3 toEye = normalize(eyePosition.xyz - input.worldPosition);

    float NdotL = dot(normal, toLight);
    float diffuseTerm = max(NdotL, 0.0f);

    float3 halfVector = normalize(toLight + toEye);
    float specularTerm = pow(max(dot(normal, halfVector), 0.0f), specularColor.a);
    // No highlight on faces facing away from the light, even if the half
    // vector math would otherwise produce one.
    specularTerm *= step(0.0f, NdotL);

    float4 texColor = g_texture.Sample(g_sampler, input.uv);
    float3 diffuseAndAmbient = texColor.rgb * (ambientColor.rgb + lightColor.rgb * diffuseTerm);
    float3 specular = lightColor.rgb * specularColor.rgb * specularTerm;

    return float4(diffuseAndAmbient + specular, texColor.a);
}

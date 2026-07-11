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

Texture2D g_diffuseTexture : register(t0);
Texture2D g_normalMap : register(t1);
SamplerState g_sampler : register(s0);

struct PSInput
{
    float4 position : SV_POSITION;
    float3 worldPosition : TEXCOORD1;
    float3 worldNormal : NORMAL;
    float3 worldTangent : TANGENT;
    float2 uv : TEXCOORD0;
};

PSInput VSMain(float3 position : POSITION, float3 normal : NORMAL, float3 tangent : TANGENT, float2 uv : TEXCOORD)
{
    PSInput result;
    result.position = mul(float4(position, 1.0f), mvp);
    result.worldPosition = mul(float4(position, 1.0f), world).xyz;
    result.worldNormal = mul(float4(normal, 0.0f), world).xyz;
    result.worldTangent = mul(float4(tangent, 0.0f), world).xyz;
    result.uv = uv;
    return result;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float3 geometricNormal = normalize(input.worldNormal);
    float3 tangent = normalize(input.worldTangent);
    float3 bitangent = cross(geometricNormal, tangent);

    // Sample the normal map (tangent-space, RGB in [0,1]) and unpack to [-1,1]
    float3 tangentSpaceNormal = g_normalMap.Sample(g_sampler, input.uv).rgb * 2.0f - 1.0f;
    float3x3 TBN = float3x3(tangent, bitangent, geometricNormal);
    float3 normal = normalize(mul(tangentSpaceNormal, TBN));

    float3 toLight = -lightDirection.xyz;
    float3 toEye = normalize(eyePosition.xyz - input.worldPosition);

    float NdotL = dot(normal, toLight);
    float diffuseTerm = max(NdotL, 0.0f);

    float3 halfVector = normalize(toLight + toEye);
    float specularTerm = pow(max(dot(normal, halfVector), 0.0f), specularColor.a);
    specularTerm *= step(0.0f, NdotL);

    float4 texColor = g_diffuseTexture.Sample(g_sampler, input.uv);
    float3 diffuseAndAmbient = texColor.rgb * (ambientColor.rgb + lightColor.rgb * diffuseTerm);
    float3 specular = lightColor.rgb * specularColor.rgb * specularTerm;

    return float4(diffuseAndAmbient + specular, texColor.a);
}

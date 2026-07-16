cbuffer SceneConstantBuffer : register(b0)
{
    matrix mvp;
    matrix world;
    matrix lightMVP;
    float4 lightDirection; // world-space, normalized (points FROM the light)
    float4 lightColor;
    float4 ambientColor;
    float4 eyePosition;    // world-space camera position (.w unused)
    float4 specularColor;  // .rgb = highlight tint, .a = shininess (specular power)
};

Texture2D g_diffuseTexture : register(t0);
Texture2D g_normalMap : register(t1);
Texture2D g_shadowMap : register(t2);
SamplerState g_sampler : register(s0);

struct PSInput
{
    float4 position : SV_POSITION;
    float3 worldPosition : TEXCOORD1;
    float3 worldNormal : NORMAL;
    float3 worldTangent : TANGENT;
    float2 uv : TEXCOORD0;
    float4 lightSpacePosition : TEXCOORD2;
};

PSInput VSMain(float3 position : POSITION, float3 normal : NORMAL, float3 tangent : TANGENT, float2 uv : TEXCOORD)
{
    PSInput result;
    result.position = mul(float4(position, 1.0f), mvp);
    result.worldPosition = mul(float4(position, 1.0f), world).xyz;
    result.worldNormal = mul(float4(normal, 0.0f), world).xyz;
    result.worldTangent = mul(float4(tangent, 0.0f), world).xyz;
    result.uv = uv;
    result.lightSpacePosition = mul(float4(position, 1.0f), lightMVP);
    return result;
}

// Simple depth-compare shadow lookup - no PCF, just one sample plus a
// small bias to fight shadow acne. Returns 1 (fully lit) or a dimmed
// constant when the pixel is farther from the light than what the shadow
// map recorded at that spot.
float SampleShadow(float4 lightSpacePosition)
{
    float3 ndc = lightSpacePosition.xyz / lightSpacePosition.w;
    float2 shadowUV = float2(ndc.x * 0.5f + 0.5f, -ndc.y * 0.5f + 0.5f);

    if (shadowUV.x < 0.0f || shadowUV.x > 1.0f || shadowUV.y < 0.0f || shadowUV.y > 1.0f)
    {
        return 1.0f; // outside the light's frustum - assume lit
    }

    const float bias = 0.0015f;
    float storedDepth = g_shadowMap.Sample(g_sampler, shadowUV).r;
    return (ndc.z - bias > storedDepth) ? 0.3f : 1.0f;
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

    float shadowFactor = SampleShadow(input.lightSpacePosition);
    diffuseTerm *= shadowFactor;
    specularTerm *= shadowFactor;

    float4 texColor = g_diffuseTexture.Sample(g_sampler, input.uv);
    float3 diffuseAndAmbient = texColor.rgb * (ambientColor.rgb + lightColor.rgb * diffuseTerm);
    float3 specular = lightColor.rgb * specularColor.rgb * specularTerm;

    return float4(diffuseAndAmbient + specular, texColor.a);
}

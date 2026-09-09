cbuffer SceneConstantBuffer : register(b0)
{
    matrix mvp;
    matrix world;
    matrix lightMVP;
    float4 lightDirection; // world-space, normalized (points FROM the light)
    float4 lightColor;
    float4 ambientColor;
    float4 eyePosition;    // world-space camera position (.w unused)
    float4 materialParams; // .x = metallic, .y = roughness
};

// The bindless table: one big array of textures instead of one fixed
// texture per register (t0/t1/t2 in steps 4-13). Which of these 16 slots
// this draw call actually uses comes from MaterialIndices below, set per
// draw via root constants (see Render()) - not from binding a different
// descriptor table per object. Array size must match
// App::BindlessHeapCapacity and the SRV range size in InitRootSignature.
Texture2D g_bindlessTextures[16] : register(t0, space0);
SamplerState g_sampler : register(s0);

cbuffer MaterialIndices : register(b1)
{
    uint diffuseTextureIndex;
    uint normalMapIndex;
    uint shadowMapIndex;
    // 0 = sample the shadow map at shadowMapIndex, 1 = read the raytraced
    // mask at shadowMaskIndex. The F key flips this at runtime.
    uint shadowMode;
    uint shadowMaskIndex;
    uint3 materialIndicesPadding;
};

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
    float storedDepth = g_bindlessTextures[shadowMapIndex].Sample(g_sampler, shadowUV).r;
    return (ndc.z - bias > storedDepth) ? 0.3f : 1.0f;
}

// The raytraced alternative. Because the mask was produced in screen space
// - one ray per pixel, written at the pixel's own coordinate - reading it
// is a straight Load with no projection, no bias, no frustum test and no
// acne. Everything SampleShadow above has to work around simply doesn't
// arise when the visibility question is answered by an actual ray.
//
// The 0.3 floor matches SampleShadow so the two techniques dim shadowed
// areas by the same amount, leaving only the *shape* of the shadow to
// differ when the F key flips between them.
float SampleRaytracedShadow(float2 screenPosition)
{
    float visibility = g_bindlessTextures[shadowMaskIndex].Load(int3(int2(screenPosition), 0)).r;
    return lerp(0.3f, 1.0f, visibility);
}

static const float PI = 3.14159265f;

// Trowbridge-Reitz/GGX normal distribution: what fraction of the surface's
// microscopic facets are angled exactly halfway between the light and the
// eye. Concentrated near NdotH=1 for low roughness (a tight, bright
// highlight); spread wide for high roughness (a dim, broad one).
float DistributionGGX(float NdotH, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0f) + 1.0f;
    return a2 / (PI * d * d + 1e-5f);
}

// Smith geometry term (Schlick-GGX form): how much light is self-shadowed
// or self-masked by the surface's own microfacets, both coming in from the
// light and going out to the eye.
float GeometrySchlickGGX(float NdotX, float k)
{
    return NdotX / (NdotX * (1.0f - k) + k);
}

float GeometrySmith(float NdotV, float NdotL, float roughness)
{
    // Direct-lighting remap (Karis 2013): k = (roughness+1)^2 / 8
    float k = (roughness + 1.0f) * (roughness + 1.0f) / 8.0f;
    return GeometrySchlickGGX(NdotV, k) * GeometrySchlickGGX(NdotL, k);
}

// Fresnel-Schlick: reflectivity rises toward 100% at grazing angles no
// matter the material - why even a dull surface looks mirror-like edge-on.
float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0f - F0) * pow(saturate(1.0f - cosTheta), 5.0f);
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float3 geometricNormal = normalize(input.worldNormal);
    float3 tangent = normalize(input.worldTangent);
    float3 bitangent = cross(geometricNormal, tangent);

    // Sample the normal map (tangent-space, RGB in [0,1]) and unpack to [-1,1].
    // normalMapIndex is dynamically uniform (the same root-constant value
    // for every pixel in this draw call), so indexing the bindless array
    // with it directly is valid - no NonUniformResourceIndex() needed. That
    // wrapper only matters when an index varies per-pixel/per-thread within
    // a single draw or dispatch (e.g. a material ID read from a G-buffer).
    float3 tangentSpaceNormal = g_bindlessTextures[normalMapIndex].Sample(g_sampler, input.uv).rgb * 2.0f - 1.0f;
    float3x3 TBN = float3x3(tangent, bitangent, geometricNormal);
    float3 N = normalize(mul(tangentSpaceNormal, TBN));

    float3 L = -lightDirection.xyz;
    float3 V = normalize(eyePosition.xyz - input.worldPosition);
    float3 H = normalize(L + V);

    float NdotL = max(dot(N, L), 0.0f);
    float NdotV = max(dot(N, V), 1e-4f);
    float NdotH = max(dot(N, H), 0.0f);
    float VdotH = max(dot(V, H), 0.0f);

    float4 texColor = g_bindlessTextures[diffuseTextureIndex].Sample(g_sampler, input.uv);
    float3 albedo = texColor.rgb;
    float metallic = materialParams.x;
    float roughness = materialParams.y;

    // Dielectrics reflect ~4% of light straight back (F0 = 0.04, roughly
    // true for plastics/stone/skin regardless of color); metals instead
    // tint their entire reflection with their own albedo and have no
    // diffuse term at all - both facts are baked into this one lerp.
    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);

    float D = DistributionGGX(NdotH, roughness);
    float G = GeometrySmith(NdotV, NdotL, roughness);
    float3 F = FresnelSchlick(VdotH, F0);

    float3 specularBRDF = (D * G * F) / max(4.0f * NdotV * NdotL, 1e-4f);

    // kS is the fraction of light that reflects specularly (F); whatever's
    // left either scatters as diffuse or - for a metal - is absorbed, so a
    // fully metallic surface (metallic = 1) ends up with no diffuse at all.
    float3 kS = F;
    float3 kD = (1.0f - kS) * (1.0f - metallic);
    float3 diffuseBRDF = kD * albedo / PI;

    float shadowFactor = (shadowMode == 0)
        ? SampleShadow(input.lightSpacePosition)
        : SampleRaytracedShadow(input.position.xy);
    float3 radiance = lightColor.rgb * shadowFactor;

    float3 directLight = (diffuseBRDF + specularBRDF) * radiance * NdotL;
    // No IBL yet (see roadmap) - a flat ambient term stands in for the
    // light bouncing in from the rest of the environment.
    float3 ambient = ambientColor.rgb * albedo;

    return float4(ambient + directLight, texColor.a);
}

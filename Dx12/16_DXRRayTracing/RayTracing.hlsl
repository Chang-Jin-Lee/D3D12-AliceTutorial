// Global root signature (see App::InitRaytracingRootSignatures) - bound
// once per command list and visible to every shader below.
RWTexture2D<float> g_shadowMask : register(u0);
RaytracingAccelerationStructure g_scene : register(t0);

cbuffer RaytracingConstants : register(b0)
{
    matrix inverseViewProjection;
    float4 cameraPosition;
    float4 lightDirection; // points FROM the light, same convention as Shaders.hlsl
};

// Local root signature - its arguments live in the shader table record
// that dispatched the hit group, not in any command list call. The cube
// record and the plane record hold different buffer addresses, so the one
// ClosestHitShader below reads whichever mesh it was actually invoked for.
struct Vertex
{
    float3 position;
    float3 normal;
    float3 tangent;
    float2 uv;
};
StructuredBuffer<Vertex> l_vertices : register(t1);
ByteAddressBuffer l_indices : register(t2);

struct RayPayload
{
    // How far along the camera ray the surface sits, or -1 if the ray
    // escaped the scene entirely.
    float hitDistance;
    float3 worldNormal;
};

struct ShadowPayload
{
    bool isLit;
};

// ByteAddressBuffer only loads 32-bit words, but the index buffers are
// R16_UINT (InitSceneGeometry). Three 16-bit indices either fill the low
// and high halves of one and a half dwords or start halfway into the
// first, depending on whether the triangle's byte offset is 4-byte
// aligned - so load two dwords from the enclosing aligned address and
// unpack accordingly. This is the standard idiom from the official
// DirectX raytracing samples.
uint3 LoadTriangleIndices(uint primitiveIndex)
{
    const uint indicesPerTriangle = 3;
    const uint bytesPerIndex = 2;
    uint offsetBytes = primitiveIndex * indicesPerTriangle * bytesPerIndex;

    uint alignedOffset = offsetBytes & ~3;
    uint2 fourIndices = l_indices.Load2(alignedOffset);

    if (alignedOffset == offsetBytes)
    {
        return uint3(fourIndices.x & 0xffff,
                     fourIndices.x >> 16,
                     fourIndices.y & 0xffff);
    }

    return uint3(fourIndices.x >> 16,
                 fourIndices.y & 0xffff,
                 fourIndices.y >> 16);
}

[shader("raygeneration")]
void RayGenShader()
{
    uint2 pixel = DispatchRaysIndex().xy;
    float2 uv = (float2(pixel) + 0.5f) / float2(DispatchRaysDimensions().xy);
    // UV origin is top-left with +Y down; NDC is centred with +Y up.
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);

    // Unproject a far-plane point to get somewhere the camera ray passes
    // through. Same matrix convention as every other shader in this
    // project: the CPU stored the transpose, so mul(vector, matrix) is a
    // row-vector multiply.
    float4 farPoint = mul(float4(ndc, 1.0f, 1.0f), inverseViewProjection);
    farPoint /= farPoint.w;

    RayDesc cameraRay;
    cameraRay.Origin = cameraPosition.xyz;
    cameraRay.Direction = normalize(farPoint.xyz - cameraPosition.xyz);
    cameraRay.TMin = 0.001f;
    cameraRay.TMax = 1000.0f;

    RayPayload payload;
    payload.hitDistance = -1.0f;
    payload.worldNormal = float3(0.0f, 1.0f, 0.0f);
    TraceRay(g_scene, RAY_FLAG_NONE, 0xFF,
             /*RayContributionToHitGroupIndex*/ 0,
             /*MultiplierForGeometryContributionToHitGroupIndex*/ 1,
             /*MissShaderIndex*/ 0,
             cameraRay, payload);

    // Nothing there: that pixel is sky, and sky is never in shadow.
    if (payload.hitDistance < 0.0f)
    {
        g_shadowMask[pixel] = 1.0f;
        return;
    }

    float3 hitPosition = cameraRay.Origin + cameraRay.Direction * payload.hitDistance;
    float3 normal = normalize(payload.worldNormal);
    float3 toLight = -normalize(lightDirection.xyz);

    // A surface angled away from the light is shadowed by its own
    // geometry. Settling that here costs one dot product and skips a ray
    // that would otherwise graze along the surface it started on.
    if (dot(normal, toLight) <= 0.0f)
    {
        g_shadowMask[pixel] = 0.0f;
        return;
    }

    RayDesc shadowRay;
    // Nudging along the normal, rather than leaning on TMin alone, is what
    // keeps a shadow ray from re-hitting the very triangle it left.
    shadowRay.Origin = hitPosition + normal * 0.01f;
    shadowRay.Direction = toLight;
    shadowRay.TMin = 0.001f;
    shadowRay.TMax = 1000.0f;

    ShadowPayload shadowPayload;
    shadowPayload.isLit = false;
    // A shadow ray only needs to know whether anything at all is in the
    // way, so it stops at the first hit and skips closest-hit entirely -
    // there's no need to find the *nearest* occluder, just any. The miss
    // shader flipping isLit to true is the only path back to lit.
    TraceRay(g_scene,
             RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
             0xFF,
             /*RayContributionToHitGroupIndex*/ 0,
             /*MultiplierForGeometryContributionToHitGroupIndex*/ 1,
             /*MissShaderIndex*/ 1,
             shadowRay, shadowPayload);

    g_shadowMask[pixel] = shadowPayload.isLit ? 1.0f : 0.0f;
}

[shader("closesthit")]
void ClosestHitShader(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attributes)
{
    uint3 indices = LoadTriangleIndices(PrimitiveIndex());
    // The hardware reports two barycentrics; the third is whatever's left.
    float3 barycentrics = float3(
        1.0f - attributes.barycentrics.x - attributes.barycentrics.y,
        attributes.barycentrics.x,
        attributes.barycentrics.y);

    float3 objectNormal =
        l_vertices[indices.x].normal * barycentrics.x +
        l_vertices[indices.y].normal * barycentrics.y +
        l_vertices[indices.z].normal * barycentrics.z;

    payload.hitDistance = RayTCurrent();
    // Every instance in this scene is a rotation plus a uniform scale, so
    // the object-to-world 3x3 rotates normals correctly on its own - no
    // inverse transpose needed.
    payload.worldNormal = mul((float3x3)ObjectToWorld3x4(), objectNormal);
}

[shader("miss")]
void MissShader(inout RayPayload payload)
{
    // Nothing was hit. RayGenShader already primed hitDistance to -1, but
    // saying so here keeps the contract in one readable place.
    payload.hitDistance = -1.0f;
}

[shader("miss")]
void ShadowMissShader(inout ShadowPayload payload)
{
    // Reaching the light without hitting anything is exactly what "lit"
    // means for a shadow ray.
    payload.isLit = true;
}

// Last stage of the bloom chain: add the blurred glow back onto the full
// HDR scene, then compress the combined HDR color down into the [0,1]
// range an 8-bit back buffer can display. Reinhard tonemapping
// (color / (color + 1)) is the simplest curve that does this - it softly
// rolls off bright values instead of hard-clipping them like a plain
// saturate() would.
Texture2D<float4> SceneTexture : register(t0);
Texture2D<float4> BloomTexture : register(t1);
RWTexture2D<float4> OutputTexture : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint width, height;
    OutputTexture.GetDimensions(width, height);
    if (dispatchThreadId.x >= width || dispatchThreadId.y >= height)
    {
        return;
    }

    float3 hdrColor = SceneTexture.Load(int3(dispatchThreadId.xy, 0)).rgb;
    float3 bloomColor = BloomTexture.Load(int3(dispatchThreadId.xy, 0)).rgb;
    float3 combined = hdrColor + bloomColor;

    float3 mapped = combined / (combined + 1.0f);
    OutputTexture[dispatchThreadId.xy] = float4(mapped, 1.0f);
}

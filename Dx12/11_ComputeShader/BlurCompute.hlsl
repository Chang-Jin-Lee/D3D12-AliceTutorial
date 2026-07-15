// Post-process box blur: reads the frame that steps 1-10 already produced
// (already MSAA-resolved to single-sample) and writes a softened copy.
// This is the first compute shader in the tutorial - no vertices, no
// rasterizer, no render target. Just a grid of GPU threads that each read
// and write one pixel.
Texture2D<float4> InputTexture : register(t0);
RWTexture2D<float4> OutputTexture : register(u0);

// One thread per pixel, in 8x8 groups. Dispatch() on the CPU side rounds
// the image size up to a multiple of 8, so a group can run partly off the
// right/bottom edge - the bounds check below discards those extra threads.
[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint width, height;
    OutputTexture.GetDimensions(width, height);
    if (dispatchThreadId.x >= width || dispatchThreadId.y >= height)
    {
        return;
    }

    // 3x3 box blur: average this pixel with its 8 neighbors. Load() reads
    // an exact texel by integer coordinate - no sampler needed, since this
    // is a plain average rather than a filtered/interpolated fetch.
    float4 sum = float4(0.0f, 0.0f, 0.0f, 0.0f);
    for (int dy = -1; dy <= 1; ++dy)
    {
        for (int dx = -1; dx <= 1; ++dx)
        {
            int2 samplePos = clamp(
                int2(dispatchThreadId.xy) + int2(dx, dy), int2(0, 0), int2(width - 1, height - 1));
            sum += InputTexture.Load(int3(samplePos, 0));
        }
    }

    OutputTexture[dispatchThreadId.xy] = sum / 9.0f;
}

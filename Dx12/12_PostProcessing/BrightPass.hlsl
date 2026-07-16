// First stage of the bloom chain: pull out only the pixels bright enough
// to "glow". The scene is rendered into an HDR (float) target, so a pixel
// lit by the boosted specular highlight in Update() can have color values
// well above 1.0 - values that would have simply clipped to white in the
// UNORM targets every earlier step used. Anything below the threshold is
// zeroed out, leaving a mostly-black image with just the bright spots,
// ready to be blurred into a glow by BlurCompute.hlsl.
Texture2D<float4> InputTexture : register(t0);
RWTexture2D<float4> OutputTexture : register(u0);

static const float kBrightnessThreshold = 1.0f;

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint width, height;
    OutputTexture.GetDimensions(width, height);
    if (dispatchThreadId.x >= width || dispatchThreadId.y >= height)
    {
        return;
    }

    float4 hdrColor = InputTexture.Load(int3(dispatchThreadId.xy, 0));
    float luminance = dot(hdrColor.rgb, float3(0.2126f, 0.7152f, 0.0722f));

    OutputTexture[dispatchThreadId.xy] =
        (luminance > kBrightnessThreshold) ? hdrColor : float4(0.0f, 0.0f, 0.0f, 0.0f);
}

import bindless;

struct BloomBlurPushConstants {
    uint src_texture_idx;
    uint dst_texture_idx;
    uint sampler_idx;
    uint is_vertical;   // 0 = horizontal pass, 1 = vertical pass
    uint2 dst_size;
    float scatter;      // only applied when is_vertical = 1
    uint accumulate;    // if 1, add scatter * result to dst instead of overwriting
};

// Binomial 5-tap Gaussian: [1, 4, 6, 4, 1] / 16
static const float kWeights[5] = { 0.0625, 0.25, 0.375, 0.25, 0.0625 };
static const int   kOffsets[5] = { -2, -1, 0, 1, 2 };

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 tid: SV_DispatchThreadID, uniform BloomBlurPushConstants pc)
{
    if (any(tid.xy >= pc.dst_size))
        return;

    float2 texel_size = 1.0 / float2(pc.dst_size);
    float2 uv = (float2(tid.xy) + 0.5) * texel_size;

    uint src_idx  = NonUniformResourceIndex(pc.src_texture_idx);
    uint samp_idx = NonUniformResourceIndex(pc.sampler_idx);

    float3 result = float3(0.0, 0.0, 0.0);

    [unroll]
    for (int i = 0; i < 5; ++i)
    {
        float2 offset = pc.is_vertical != 0u
            ? float2(0.0, float(kOffsets[i]) * texel_size.y)
            : float2(float(kOffsets[i]) * texel_size.x, 0.0);

        result += sampled_images[src_idx]
            .SampleLevel(samplers[samp_idx], uv + offset, 0.0).rgb * kWeights[i];
    }

    if (pc.accumulate != 0u)
    {
        float3 existing = storage_images[NonUniformResourceIndex(pc.dst_texture_idx)][tid.xy].rgb;
        storage_images[NonUniformResourceIndex(pc.dst_texture_idx)][tid.xy] = float4(existing + result * pc.scatter, 1.0);
    }
    else
    {
        storage_images[NonUniformResourceIndex(pc.dst_texture_idx)][tid.xy] = float4(result, 1.0);
    }
}

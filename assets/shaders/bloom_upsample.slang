import bindless;

struct BloomUpsamplePushConstants {
    uint src_texture_idx;
    uint dst_texture_idx;
    uint sampler_idx;
    float scatter;
    uint2 dst_size;
    uint2 _pad;
};

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 tid: SV_DispatchThreadID, uniform BloomUpsamplePushConstants pc)
{
    if (any(tid.xy >= pc.dst_size))
        return;

    float2 uv = (float2(tid.xy) + 0.5) / float2(pc.dst_size);
    float2 src_texel_size = 2.0 / float2(pc.dst_size);

    uint src_idx  = NonUniformResourceIndex(pc.src_texture_idx);
    uint samp_idx = NonUniformResourceIndex(pc.sampler_idx);

    float3 result = float3(0.0, 0.0, 0.0);

    result += sampled_images[src_idx].SampleLevel(samplers[samp_idx], uv + src_texel_size * float2(-1.0, -1.0), 0.0).rgb * (1.0 / 16.0);
    result += sampled_images[src_idx].SampleLevel(samplers[samp_idx], uv + src_texel_size * float2( 0.0, -1.0), 0.0).rgb * (2.0 / 16.0);
    result += sampled_images[src_idx].SampleLevel(samplers[samp_idx], uv + src_texel_size * float2( 1.0, -1.0), 0.0).rgb * (1.0 / 16.0);
    result += sampled_images[src_idx].SampleLevel(samplers[samp_idx], uv + src_texel_size * float2(-1.0,  0.0), 0.0).rgb * (2.0 / 16.0);
    result += sampled_images[src_idx].SampleLevel(samplers[samp_idx], uv + src_texel_size * float2( 0.0,  0.0), 0.0).rgb * (4.0 / 16.0);
    result += sampled_images[src_idx].SampleLevel(samplers[samp_idx], uv + src_texel_size * float2( 1.0,  0.0), 0.0).rgb * (2.0 / 16.0);
    result += sampled_images[src_idx].SampleLevel(samplers[samp_idx], uv + src_texel_size * float2(-1.0,  1.0), 0.0).rgb * (1.0 / 16.0);
    result += sampled_images[src_idx].SampleLevel(samplers[samp_idx], uv + src_texel_size * float2( 0.0,  1.0), 0.0).rgb * (2.0 / 16.0);
    result += sampled_images[src_idx].SampleLevel(samplers[samp_idx], uv + src_texel_size * float2( 1.0,  1.0), 0.0).rgb * (1.0 / 16.0);

    float3 existing = storage_images[NonUniformResourceIndex(pc.dst_texture_idx)][tid.xy].rgb;
    storage_images[NonUniformResourceIndex(pc.dst_texture_idx)][tid.xy] = float4(existing + result * pc.scatter, 1.0);
}

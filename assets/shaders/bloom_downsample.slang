import bindless;

struct BloomDownsamplePushConstants {
    uint src_texture_idx;
    uint dst_texture_idx;
    uint sampler_idx;
    uint is_first_pass;
    uint2 src_size;
    float threshold;
    float _pad;
};

static const float3 kLumaWeights = float3(0.2126, 0.7152, 0.0722);

float4 sample_src(uint src_idx, uint samp_idx, float2 uv)
{
    return sampled_images[NonUniformResourceIndex(src_idx)]
        .SampleLevel(samplers[NonUniformResourceIndex(samp_idx)], uv, 0.0);
}

// Alpha-weighted Karis average: discarded pixels (alpha=0) from alpha-tested
// geometry don't bleed brightness into the bloom texture.
float3 karis_average(float4 a, float4 b, float4 c, float4 d)
{
    float wa = a.a / (1.0 + dot(a.rgb, kLumaWeights));
    float wb = b.a / (1.0 + dot(b.rgb, kLumaWeights));
    float wc = c.a / (1.0 + dot(c.rgb, kLumaWeights));
    float wd = d.a / (1.0 + dot(d.rgb, kLumaWeights));
    float total = wa + wb + wc + wd;
    if (total < 1e-6)
        return float3(0.0);
    return (a.rgb * wa + b.rgb * wb + c.rgb * wc + d.rgb * wd) / total;
}

float3 apply_threshold(float3 color, float threshold)
{
    float luma = dot(color, kLumaWeights);
    float soft_knee = threshold * 0.5;
    float rq = clamp(luma - threshold + soft_knee, 0.0, 2.0 * soft_knee);
    rq = (rq * rq) / (4.0 * soft_knee + 0.00001);
    float weight = max(rq, luma - threshold) / max(luma, 0.00001);
    return color * weight;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 tid: SV_DispatchThreadID, uniform BloomDownsamplePushConstants pc)
{
    uint2 dst_size = (pc.src_size + 1u) / 2u;
    if (any(tid.xy >= dst_size))
        return;

    float2 texel_size = 1.0 / float2(pc.src_size);
    float2 uv = (float2(tid.xy) + 0.5) * 2.0 * texel_size;

    float3 result;

    if (pc.is_first_pass != 0u)
    {
        float4 s0 = sample_src(pc.src_texture_idx, pc.sampler_idx, uv + texel_size * float2(-1.0, -1.0));
        float4 s1 = sample_src(pc.src_texture_idx, pc.sampler_idx, uv + texel_size * float2( 1.0, -1.0));
        float4 s2 = sample_src(pc.src_texture_idx, pc.sampler_idx, uv + texel_size * float2(-1.0,  1.0));
        float4 s3 = sample_src(pc.src_texture_idx, pc.sampler_idx, uv + texel_size * float2( 1.0,  1.0));

        result = karis_average(s0, s1, s2, s3);
        result = apply_threshold(result, pc.threshold);
    }
    else
    {
        float2 ts = texel_size;

        float3 a = sample_src(pc.src_texture_idx, pc.sampler_idx, uv + ts * float2(-2.0, -2.0)).rgb;
        float3 b = sample_src(pc.src_texture_idx, pc.sampler_idx, uv + ts * float2( 0.0, -2.0)).rgb;
        float3 c = sample_src(pc.src_texture_idx, pc.sampler_idx, uv + ts * float2( 2.0, -2.0)).rgb;
        float3 d = sample_src(pc.src_texture_idx, pc.sampler_idx, uv + ts * float2(-2.0,  0.0)).rgb;
        float3 e = sample_src(pc.src_texture_idx, pc.sampler_idx, uv + ts * float2( 0.0,  0.0)).rgb;
        float3 f = sample_src(pc.src_texture_idx, pc.sampler_idx, uv + ts * float2( 2.0,  0.0)).rgb;
        float3 g = sample_src(pc.src_texture_idx, pc.sampler_idx, uv + ts * float2(-2.0,  2.0)).rgb;
        float3 h = sample_src(pc.src_texture_idx, pc.sampler_idx, uv + ts * float2( 0.0,  2.0)).rgb;
        float3 i = sample_src(pc.src_texture_idx, pc.sampler_idx, uv + ts * float2( 2.0,  2.0)).rgb;
        float3 j = sample_src(pc.src_texture_idx, pc.sampler_idx, uv + ts * float2(-1.0, -1.0)).rgb;
        float3 k = sample_src(pc.src_texture_idx, pc.sampler_idx, uv + ts * float2( 1.0, -1.0)).rgb;
        float3 l = sample_src(pc.src_texture_idx, pc.sampler_idx, uv + ts * float2(-1.0,  1.0)).rgb;
        float3 m = sample_src(pc.src_texture_idx, pc.sampler_idx, uv + ts * float2( 1.0,  1.0)).rgb;

        result  = (a + c + g + i) * 0.03125;
        result += (b + d + f + h) * 0.0625;
        result += (j + k + l + m) * 0.125;
        result += e                * 0.125;
    }

    storage_images[NonUniformResourceIndex(pc.dst_texture_idx)][tid.xy] = float4(result, 1.0);
}

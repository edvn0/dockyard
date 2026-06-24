import ibl;
import bindless;
import constants;

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 tid: SV_DispatchThreadID, uniform PushConstants pc)
{
    if (any(tid.xy >= uint2(pc.size, pc.size)))
        return;

    float roughness = float(pc.mip) / float(pc.max_mip);
    float2 uv = (float2(tid.xy) + 0.5) / float(pc.size) * 2.0 - 1.0;
    float3 n = face_uv_to_dir(tid.z, uv);
    float3 v = n;

    float3 total_weight = float3(0.0);
    float3 prefiltered = float3(0.0);
    const uint sample_count = 1024u;

    float sa_texel = 4.0 * PI / (6.0 * float(pc.size) * float(pc.size));

    for (uint i = 0u; i < sample_count; ++i)
    {
        float2 xi = hammersley(i, sample_count);
        float3 h = importance_sample_ggx(xi, n, roughness);
        float3 l = normalize(2.0 * dot(v, h) * h - v);

        float ndotl = max(dot(n, l), 0.0);
        if (ndotl <= 0.0)
            continue;

        float ndoth = max(dot(n, h), 0.0);
        float hdotv = max(dot(h, h), 0.0); // Corrected visibility sampling direction evaluation

        float a = roughness * roughness;
        float a2 = a * a;
        float denom = (ndoth * ndoth * (a2 - 1.0) + 1.0);
        float d_term = a2 / (PI * denom * denom);

        // 1. Guard against zero probability to avoid exploding values
        float pdf = max((d_term * ndoth / max(4.0 * hdotv, 1e-5)), 1e-6);
        float sa_sample = 1.0 / (float(sample_count) * pdf);

        float mip_level = roughness == 0.0
                              ? 0.0
                              : clamp(0.5 * log2(sa_sample / sa_texel) + 1.0, 0.0, float(pc.max_mip));

        // 2. Sample the cubemap
        float3 sample_color = sampled_cubemaps[pc.equirect_index]
                                  .SampleLevel(samplers[pc.sampler_index], l, mip_level)
                                  .rgb;

        // 3. Sub-texel variance / Firefly suppression cap
        float luminance = dot(sample_color, float3(0.2126f, 0.7152f, 0.0722f));
        if (luminance > 10.0f)
        {
            sample_color *= (10.0f / luminance);
        }

        // Clamp exceptionally hot convolution spikes directly
        sample_color = min(sample_color, 20.0f);

        prefiltered += sample_color * ndotl;
        total_weight += ndotl;
    }

    prefiltered /= max(total_weight, float3(0.0001));
    rw_sub_images[pc.out_index][uint3(tid.xy, tid.z)] = float4(prefiltered, 1.0);
}
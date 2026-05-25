import include.ibl;
import bindless;

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 tid: SV_DispatchThreadID, uniform PushConstants pc)
{
    if (any(tid.xy >= uint2(pc.size, pc.size)))
        return;

    float roughness = float(pc.mip) / float(pc.max_mip);
    float2 uv = (float2(tid.xy) + 0.5) / float(pc.size) * 2.0 - 1.0;
    float3 n = face_uv_to_dir(tid.z, uv);
    float3 v = n; // assume V == R == N (split-sum approximation)

    float3 total_weight = float3(0.0);
    float3 prefiltered = float3(0.0);
    const uint sample_count = 1024u;

    for (uint i = 0u; i < sample_count; ++i)
    {
        float2 xi = hammersley(i, sample_count);
        float3 h = importance_sample_ggx(xi, n, roughness);
        float3 l = normalize(2.0 * dot(v, h) * h - v);

        float ndotl = max(dot(n, l), 0.0);
        if (ndotl > 0.0)
        {
            prefiltered += sampled_cubemaps[pc.equirect_index]
                               .SampleLevel(samplers[pc.sampler_index], l, 0.0)
                               .rgb *
                           ndotl;
            total_weight += ndotl;
        }
    }

    prefiltered /= total_weight;
    rw_sub_images[pc.out_index][uint3(tid.xy, tid.z)] = float4(prefiltered, 1.0);
}

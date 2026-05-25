import include.ibl;
import bindless;
import include.pbr;

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 tid: SV_DispatchThreadID, uniform PushConstants pc)
{
    if (any(tid.xy >= uint2(pc.size, pc.size)))
        return;

    float ndotv = (float(tid.x) + 0.5) / float(pc.size);

    float roughness = 1.0 - ((float(tid.y) + 0.5) / float(pc.size));

    float3 v = float3(sqrt(max(1.0 - ndotv * ndotv, 0.0)), 0.0, ndotv);
    float3 n = float3(0.0, 0.0, 1.0);

    float scale = 0.0;
    float bias = 0.0;
    const uint sample_count = 1024u;

    for (uint i = 0u; i < sample_count; ++i)
    {
        float2 xi = hammersley(i, sample_count);
        float3 h = importance_sample_ggx(xi, n, roughness);

        float3 l = normalize(2.0 * dot(v, h) * h - v);

        float ndotl = max(l.z, 0.0);
        float ndoth = max(h.z, 0.0);
        float vdoth = max(dot(v, h), 0.0);

        if (ndotl > 0.0)
        {
            float g = geometry_smith(n, v, l, roughness);

            float g_vis = (g * vdoth) / (ndoth * ndotv * ndotl + 1e-4);

            float fc = pow(1.0 - vdoth, 5.0);

            scale += (1.0 - fc) * g_vis;
            bias += fc * g_vis;
        }
    }

    scale /= float(sample_count);
    bias /= float(sample_count);

    storage_images[pc.out_index][tid.xy] = float4(scale, bias, 0.0, 1.0);
}
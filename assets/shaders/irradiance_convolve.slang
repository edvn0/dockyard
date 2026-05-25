import include.ibl;
import bindless;
import constants;

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 tid: SV_DispatchThreadID, uniform PushConstants pc)
{
    if (any(tid.xy >= uint2(pc.size, pc.size)))
        return;

    float2 uv = (float2(tid.xy) + 0.5) / float(pc.size) * 2.0 - 1.0;
    float3 n = face_uv_to_dir(tid.z, uv); // normal for this texel

    // Build TBN around n for hemisphere sampling
    float3 up = abs(n.y) < 0.999 ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 right = normalize(cross(up, n));
    float3 fwd = cross(n, right);

    float3 irradiance = float3(0.0);
    uint samples = 0u;
    float delta = 0.025; // ~5700 samples

    for (float phi = 0.0; phi < 2.0 * PI; phi += delta)
    {
        for (float theta = 0.0; theta < 0.5 * PI; theta += delta)
        {
            float3 local = float3(sin(theta) * cos(phi),
                                  sin(theta) * sin(phi),
                                  cos(theta));
            float3 world = local.x * right + local.y * fwd + local.z * n;

            irradiance += sampled_cubemaps[pc.equirect_index]
                              .SampleLevel(samplers[pc.sampler_index], world, 0.0)
                              .rgb *
                          cos(theta) * sin(theta);
            ++samples;
        }
    }

    irradiance = PI * irradiance / float(samples);

    rw_sub_images[pc.out_index][uint3(tid.xy, tid.z)] = float4(irradiance, 1.0);
}

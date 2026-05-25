import include.ibl;
import bindless;

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 tid: SV_DispatchThreadID, uniform PushConstants pc)
{
    if (any(tid.xy >= uint2(pc.size, pc.size)))
        return;

    float2 uv = (float2(tid.xy) + 0.5) / float(pc.size) * 2.0 - 1.0;
    float3 dir = face_uv_to_dir(tid.z, uv);

    float2 eq_uv = dir_to_equirect_uv(dir);
    float4 color = sampled_images[pc.equirect_index]
                       .SampleLevel(samplers[pc.sampler_index], eq_uv, 0.0);

    // Write to face layer of the output cubemap storage view
    RWTexture2DArray<float4> out_cube = rw_sub_images[pc.out_index];
    out_cube[uint3(tid.xy, tid.z)] = color;
}

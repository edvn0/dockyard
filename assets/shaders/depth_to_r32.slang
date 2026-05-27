import bindless;

struct DepthToR32PushConstants
{
    uint src_depth_idx;
    uint dst_color_idx;
    uint2 src_dimension;
};

[shader("compute")]
[numthreads(16, 16, 1)]
void computeMain(uint3 dispatchThreadID : SV_DispatchThreadID,
                 uniform DepthToR32PushConstants pc)
{
    uint2 coord = dispatchThreadID.xy;
    if (coord.x >= pc.src_dimension.x || coord.y >= pc.src_dimension.y)
        return;

    float d = sampled_images[NonUniformResourceIndex(pc.src_depth_idx)]
                  .Load(int3(coord, 0)).x;
    storage_images[NonUniformResourceIndex(pc.dst_color_idx)][coord] =
        float4(d, 0.0f, 0.0f, 0.0f);
}
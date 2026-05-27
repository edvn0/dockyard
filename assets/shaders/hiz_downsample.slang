import bindless;

struct HizPushConstants
{
    uint src_texture_idx;
    uint dst_texture_idx;
    uint2 src_dimension;
};

[shader("compute")]
[numthreads(16, 16, 1)]
void computeMain(uint3 dispatchThreadID: SV_DispatchThreadID, uniform HizPushConstants pc)
{
    uint2 dst_coord = dispatchThreadID.xy;
    uint2 src_coord = dst_coord * 2;

    if (src_coord.x >= pc.src_dimension.x || src_coord.y >= pc.src_dimension.y)
        return;

    uint src_idx = NonUniformResourceIndex(pc.src_texture_idx);
    float d0 = sampled_images[src_idx].Load(int3(src_coord, 0)).x;
    float d1 = sampled_images[src_idx].Load(int3(src_coord + uint2(1, 0), 0)).x;
    float d2 = sampled_images[src_idx].Load(int3(src_coord + uint2(0, 1), 0)).x;
    float d3 = sampled_images[src_idx].Load(int3(src_coord + uint2(1, 1), 0)).x;

    float conservative_depth = min(min(d0, d1), min(d2, d3));

    uint dst_idx = NonUniformResourceIndex(pc.dst_texture_idx);
    storage_images[dst_idx][dst_coord] = float4(conservative_depth, 0.0f, 0.0f, 0.0f);
}

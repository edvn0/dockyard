import bindless;

struct HizPushConstants
{
    uint src_texture_idx;
    uint dst_texture_idx;
    uint2 src_dimension;
};

[shader("compute")]
[numthreads(16, 16, 1)]
void computeMain(uint3 tid: SV_DispatchThreadID, uniform HizPushConstants pc)
{
    uint2 dst_coord = tid.xy;
    uint2 dst_dim = (pc.src_dimension + 1u) / 2u;
    if (any(dst_coord >= dst_dim))
        return;

    uint2 s = dst_coord * 2u;
    uint2 s_max = pc.src_dimension - 1u;

    uint src_idx = NonUniformResourceIndex(pc.src_texture_idx);
    float d0 = sampled_images[src_idx].Load(int3(min(s,                      s_max), 0)).x;
    float d1 = sampled_images[src_idx].Load(int3(min(s + uint2(1, 0),        s_max), 0)).x;
    float d2 = sampled_images[src_idx].Load(int3(min(s + uint2(0, 1),        s_max), 0)).x;
    float d3 = sampled_images[src_idx].Load(int3(min(s + uint2(1, 1),        s_max), 0)).x;

    float conservative_depth = min(min(d0, d1), min(d2, d3));
    storage_images[NonUniformResourceIndex(pc.dst_texture_idx)][dst_coord] =
        float4(conservative_depth, 0.0f, 0.0f, 0.0f);
}

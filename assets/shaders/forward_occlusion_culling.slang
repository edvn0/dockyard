import push_constant;
import bindless;

struct OcclusionCullingPushConstants
{
    ImmutablePtr<InstanceData, AddressSpace.Device> instance_buffer;
    ImmutablePtr<FrameUBO, AddressSpace.Device> frame_data;

    ImmutablePtr<uint, AddressSpace.Device> forward_original_remap;
    ImmutablePtr<uint, AddressSpace.Device> forward_instance_to_command;
    Ptr<VkDrawIndexedIndirectCommand, Access.ReadWrite, AddressSpace.Device> forward_indirect_commands;
    Ptr<uint, Access.ReadWrite, AddressSpace.Device> forward_culled_remap;

    uint total_instance_count;
    uint hiz_sampler_idx;
    float2 hiz_dimensions;
    uint hiz_mip_indices[16]; // index 16 is total mip count
};
static const float lod_thresholds_sq[5] = {
    0.12f * 0.12f,   // LOD0 → LOD1
    0.07f * 0.07f,   // LOD1 → LOD2
    0.04f * 0.04f,   // LOD2 → LOD3
    0.02f * 0.02f,   // LOD3 → LOD4
    0.008f * 0.008f, // LOD4 → LOD5
};

uint select_lod(float radius, float dist_sq, uint lod_count)
{
    if (lod_count <= 1u || dist_sq < 1e-6f)
        return 0;
    float ratio_sq = (radius * radius) / dist_sq;
    for (uint i = 0; i < lod_count - 1; ++i)
    {
        if (ratio_sq >= lod_thresholds_sq[i])
            return i;
    }
    return lod_count - 1;
}

bool is_visible(float4x4 model_matrix, float local_radius, Array<float4, 6> frustum_planes, float3 camera_pos, float max_dist_sq)
{
    float3 position = float3(model_matrix[0][3], model_matrix[1][3], model_matrix[2][3]);

    float scale_x = length(float3(model_matrix[0][0], model_matrix[1][0], model_matrix[2][0]));
    float scale_y = length(float3(model_matrix[0][1], model_matrix[1][1], model_matrix[2][1]));
    float scale_z = length(float3(model_matrix[0][2], model_matrix[1][2], model_matrix[2][2]));

    float max_scale = max(scale_x, max(scale_y, scale_z));
    float world_radius = local_radius * max_scale;

    [ForceUnroll]
    for (int i = 0; i < 6; ++i)
    {
        if (dot(float4(position, 1.0f), frustum_planes[i]) < -world_radius)
        {
            return false;
        }
    }

    float3 diff = position - camera_pos;
    float dist_sq = dot(diff, diff);

    if (dist_sq > max_dist_sq)
    {
        return false;
    }

    return true;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 dispatchThreadID: SV_DispatchThreadID, uniform OcclusionCullingPushConstants pc)
{
    uint item_idx = dispatchThreadID.x;
    if (item_idx >= pc.total_instance_count)
        return;

    var instance = unpack_instance_data(pc.instance_buffer[item_idx]);

    bool visible = is_visible(instance.transform, instance.bounding_radius,
                              pc.frame_data.frustum_planes,
                              pc.frame_data.camera_pos.xyz, 100.0f * 100.0f);

    float3 world_pos = float3(instance.transform[0][3], instance.transform[1][3], instance.transform[2][3]);
    float dist_sq = dot(world_pos - pc.frame_data.camera_pos.xyz, world_pos - pc.frame_data.camera_pos.xyz);
    uint lod_idx = select_lod(instance.bounding_radius, dist_sq, instance.lod_count);

    if (visible)
    {
        float3 view_pos = mul(pc.frame_data.view, float4(world_pos, 1.0f)).xyz;

        if (view_pos.z - instance.bounding_radius < 0.0f)
        {
            float3 min_box = view_pos - float3(instance.bounding_radius);
            float3 max_box = view_pos + float3(instance.bounding_radius);

            float3 corners[8] = {
                float3(min_box.x, min_box.y, min_box.z), float3(max_box.x, min_box.y, min_box.z),
                float3(min_box.x, max_box.y, min_box.z), float3(max_box.x, max_box.y, min_box.z),
                float3(min_box.x, min_box.y, max_box.z), float3(max_box.x, min_box.y, max_box.z),
                float3(min_box.x, max_box.y, max_box.z), float3(max_box.x, max_box.y, max_box.z)
            };

            float3 min_ndc = float3(1e5f);
            float3 max_ndc = float3(-1e5f);

            [ForceUnroll]
            for (int i = 0; i < 8; ++i)
            {
                float4 clip = mul(pc.frame_data.projection, float4(corners[i], 1.0f));
                float3 ndc = clip.xyz / clip.w;
                min_ndc = min(min_ndc, ndc);
                max_ndc = max(max_ndc, ndc);
            }

            float2 min_uv = clamp(min_ndc.xy * 0.5f + 0.5f, 0.0f, 1.0f);
            float2 max_uv = clamp(max_ndc.xy * 0.5f + 0.5f, 0.0f, 1.0f);

            float flipped_min_y = 1.0f - max_uv.y;
            float flipped_max_y = 1.0f - min_uv.y;
            min_uv.y = flipped_min_y;
            max_uv.y = flipped_max_y;

            float2 size_pixels = abs(max_uv - min_uv) * pc.hiz_dimensions;
            float mip_count = float(pc.hiz_mip_indices[15]);
            float mip = clamp(ceil(log2(max(size_pixels.x, size_pixels.y))), 0.0f, mip_count - 1.0f);

            uint target_view_idx = pc.hiz_mip_indices[uint(mip)];

            float2 center_uv = (min_uv + max_uv) * 0.5f;
            float4 depth_samples = sampled_images[NonUniformResourceIndex(target_view_idx)]
                                       .GatherRed(samplers[NonUniformResourceIndex(pc.hiz_sampler_idx)], center_uv);

            float max_visible_hiz_depth = min(min(depth_samples.x, depth_samples.y), min(depth_samples.z, depth_samples.w));

            float closest_depth = max_ndc.z;
            if (closest_depth < max_visible_hiz_depth)
            {
                visible = false;
            }
        }
    }

    if (visible)
    {
        uint forward_base_cmd_id = pc.forward_instance_to_command[item_idx];
        if (forward_base_cmd_id != 0xFFFFFFFF)
        {
            uint forward_lod_cmd_id = forward_base_cmd_id + lod_idx;
            Ptr<VkDrawIndexedIndirectCommand, Access.ReadWrite, AddressSpace.Device> cmd_ptr =
                pc.forward_indirect_commands + forward_lod_cmd_id;

            uint forward_slot;
            InterlockedAdd((*cmd_ptr).instanceCount, 1, forward_slot);
            pc.forward_culled_remap[(*cmd_ptr).firstInstance + forward_slot] = item_idx | (lod_idx << 28u);
        }
    }
}

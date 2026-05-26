import push_constant;

struct CullingPushConstants
{

    ImmutablePtr<InstanceData, AddressSpace.Device> instance_buffer;
    ImmutablePtr<FrameUBO, AddressSpace.Device> frame_data;

    ImmutablePtr<uint, AddressSpace.Device> depth_original_remap;
    ImmutablePtr<uint, AddressSpace.Device> depth_instance_to_command;
    Ptr<VkDrawIndexedIndirectCommand, Access.ReadWrite, AddressSpace.Device> depth_indirect_commands;
    Ptr<uint, Access.ReadWrite, AddressSpace.Device> depth_culled_remap;

    ImmutablePtr<uint, AddressSpace.Device> forward_original_remap;
    ImmutablePtr<uint, AddressSpace.Device> forward_instance_to_command;
    Ptr<VkDrawIndexedIndirectCommand, Access.ReadWrite, AddressSpace.Device> forward_indirect_commands;
    Ptr<uint, Access.ReadWrite, AddressSpace.Device> forward_culled_remap;

    uint total_instance_count;
    uint padding[3];
};

static const float lod_thresholds_sq[5] = {
    0.12f * 0.12f,   // LOD0 → LOD1
    0.07f * 0.07f,   // LOD1 → LOD2
    0.04f * 0.04f,   // LOD2 → LOD3
    0.02f * 0.02f,   // LOD3 → LOD4
    0.008f * 0.008f, // LOD4 → LOD5
};

uint select_lod(float radius, float dist_sq, uint lod_count) {
    if (lod_count <= 1u || dist_sq < 1e-6f) return 0;
    float ratio_sq = (radius * radius) / dist_sq;
    for (uint i = 0; i < lod_count - 1; ++i) {
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
void computeMain(uint3 dispatchThreadID: SV_DispatchThreadID, uniform CullingPushConstants pc)
{
    uint item_idx = dispatchThreadID.x;
    if (item_idx >= pc.total_instance_count)
        return;

    var instance = unpack_instance_data(pc.instance_buffer[item_idx]);

    float3 world_pos = float3(instance.transform[0][3], instance.transform[1][3], instance.transform[2][3]);
    float dist_sq = dot(world_pos - pc.frame_data.camera_pos.xyz,
                        world_pos - pc.frame_data.camera_pos.xyz);
    uint lod_idx = select_lod(instance.bounding_radius, dist_sq, instance.lod_count);

    // Depth prepass + shadow maps: always pass, still LOD-select
    uint depth_base_cmd_id = pc.depth_instance_to_command[item_idx];
    if (depth_base_cmd_id != 0xFFFFFFFF)
    {
        uint depth_lod_cmd_id = depth_base_cmd_id + lod_idx;
        Ptr<VkDrawIndexedIndirectCommand, Access.ReadWrite, AddressSpace.Device> cmd_ptr =
            pc.depth_indirect_commands + depth_lod_cmd_id;

        uint depth_slot;
        InterlockedAdd((*cmd_ptr).instanceCount, 1, depth_slot);
        pc.depth_culled_remap[(*cmd_ptr).firstInstance + depth_slot] = item_idx;
    }

    // Forward pass: frustum + distance culled
    bool visible = is_visible(instance.transform, instance.bounding_radius,
                              pc.frame_data.frustum_planes,
                              pc.frame_data.camera_pos.xyz, 100.0f * 100.0f);
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
            pc.forward_culled_remap[(*cmd_ptr).firstInstance + forward_slot] =
                item_idx | (lod_idx << 28u);
        }
    }
}

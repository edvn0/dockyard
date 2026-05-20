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

bool is_visible(float4x4 model_matrix, float radius, Array<float4, 6> frustum_planes, float3 camera_pos, float max_dist_sq)
{
    float3 position = float3(model_matrix[0][3], model_matrix[1][3], model_matrix[2][3]);

    for (int i = 0; i < 6; ++i)
    {
        if (dot(float4(position, 1.0f), frustum_planes[i]) < -radius)
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

    InstanceData instance = pc.instance_buffer[item_idx];

    bool visible = is_visible(instance.transform, instance.bounding_radius, pc.frame_data.frustum_planes, pc.frame_data.camera_pos.xyz, 50.0f * 50.0f);

    if (visible)
    {
        uint depth_cmd_id = pc.depth_instance_to_command[item_idx];

        if (depth_cmd_id != 0xFFFFFFFF)
        {
            Ptr<VkDrawIndexedIndirectCommand, Access.ReadWrite, AddressSpace.Device> cmd_ptr =
                pc.depth_indirect_commands + depth_cmd_id;
            uint depth_slot;
            InterlockedAdd((*cmd_ptr).instanceCount, 1, depth_slot);
            uint depth_dest = (*cmd_ptr).firstInstance + depth_slot;
            pc.depth_culled_remap[depth_dest] = item_idx;
        }

        uint forward_cmd_id = pc.forward_instance_to_command[item_idx];

        if (forward_cmd_id != 0xFFFFFFFF)
        {
            Ptr<VkDrawIndexedIndirectCommand, Access.ReadWrite, AddressSpace.Device> cmd_ptr =
                pc.forward_indirect_commands + forward_cmd_id;
            uint forward_slot;
            InterlockedAdd((*cmd_ptr).instanceCount, 1, forward_slot);
            uint forward_dest = (*cmd_ptr).firstInstance + forward_slot;
            pc.forward_culled_remap[forward_dest] = item_idx;
        }
    }
}

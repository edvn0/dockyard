import push_constant;
import culling_common;

struct DepthCullingPushConstants {
  ImmutablePtr<InstanceData, AddressSpace.Device> instance_buffer;
  ImmutablePtr<FrameUBO, AddressSpace.Device> frame_data;

  ImmutablePtr<uint, AddressSpace.Device> depth_original_remap;
  ImmutablePtr<uint, AddressSpace.Device> depth_instance_to_command;
  Ptr<VkDrawIndexedIndirectCommand, Access.ReadWrite, AddressSpace.Device>
      depth_indirect_commands;
  Ptr<uint, Access.ReadWrite, AddressSpace.Device> depth_culled_remap;

  uint total_instance_count;
  uint padding[3];
};

[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 dispatchThreadID: SV_DispatchThreadID,
                 uniform DepthCullingPushConstants pc) {
  uint item_idx = dispatchThreadID.x;
  if (item_idx >= pc.total_instance_count)
    return;

  var instance = unpack_instance_data(pc.instance_buffer[item_idx]);

  let cull = cull_instance(instance.transform, instance.bounding_radius,
                           instance.lod_count, pc.frame_data.frustum_planes,
                           pc.frame_data.camera_pos.xyz,
                           pc.frame_data.camera_far * pc.frame_data.camera_far);

  if (!cull.visible)
    return;

  uint depth_base_cmd_id = pc.depth_instance_to_command[item_idx];
  if (depth_base_cmd_id != 0xFFFFFFFF) {
    uint depth_lod_cmd_id = depth_base_cmd_id + cull.lod_idx;
    Ptr<VkDrawIndexedIndirectCommand, Access.ReadWrite, AddressSpace.Device>
        cmd_ptr = pc.depth_indirect_commands + depth_lod_cmd_id;

    uint depth_slot;
    InterlockedAdd((*cmd_ptr).instanceCount, 1, depth_slot);
    pc.depth_culled_remap[(*cmd_ptr).firstInstance + depth_slot] = item_idx;
  }
}

import push_constant;
import bindless;
import culling_common;

struct OcclusionCullingPushConstants {
  ImmutablePtr<InstanceData, AddressSpace.Device> instance_buffer;
  ImmutablePtr<FrameUBO, AddressSpace.Device> frame_data;

  ImmutablePtr<uint, AddressSpace.Device> forward_instance_to_command;
  Ptr<VkDrawIndexedIndirectCommand, Access.ReadWrite, AddressSpace.Device>
      forward_indirect_commands;
  Ptr<uint, Access.ReadWrite, AddressSpace.Device> forward_culled_remap;

  uint total_instance_count;
  uint hiz_sampler_idx;
  float hiz_width;
  float hiz_height;
  uint4 hiz_mip_indices[4];
};

uint hiz_mip_index(OcclusionCullingPushConstants pc, uint flat) {
  return pc.hiz_mip_indices[flat >> 2u][flat & 3u];
}

[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 dispatchThreadID: SV_DispatchThreadID,
                 uniform OcclusionCullingPushConstants pc) {
  uint item_idx = dispatchThreadID.x;
  if (item_idx >= pc.total_instance_count)
    return;

  var instance = unpack_instance_data(pc.instance_buffer[item_idx]);

  let cull = cull_instance(instance.transform, instance.bounding_radius,
                           instance.lod_count, pc.frame_data.frustum_planes,
                           pc.frame_data.camera_pos.xyz,
                           pc.frame_data.camera_far * pc.frame_data.camera_far);

  bool visible = cull.visible;

  if (visible) {
    float3 world_pos =
        float3(instance.transform[0][3], instance.transform[1][3],
               instance.transform[2][3]);
    float3 view_pos = mul(pc.frame_data.view, float4(world_pos, 1.0f)).xyz;

    if (view_pos.z - instance.bounding_radius > 0.0f) {
      float3 min_box = view_pos - float3(instance.bounding_radius);
      float3 max_box = view_pos + float3(instance.bounding_radius);

      float3 corners[8] = { float3(min_box.x, min_box.y, min_box.z),
                            float3(max_box.x, min_box.y, min_box.z),
                            float3(min_box.x, max_box.y, min_box.z),
                            float3(max_box.x, max_box.y, min_box.z),
                            float3(min_box.x, min_box.y, max_box.z),
                            float3(max_box.x, min_box.y, max_box.z),
                            float3(min_box.x, max_box.y, max_box.z),
                            float3(max_box.x, max_box.y, max_box.z) };

      float3 min_ndc = float3(1e5f);
      float3 max_ndc = float3(-1e5f);
      bool near_clipped = false;

      [ForceUnroll]
      for (int i = 0; i < 8; ++i) {
        float4 clip = mul(pc.frame_data.projection, float4(corners[i], 1.0f));
        near_clipped = near_clipped || (clip.w <= 0.0f);
        float w_safe = clip.w > 0.0f ? clip.w : 1.0f;
        float3 ndc = clip.xyz / w_safe;
        min_ndc = min(min_ndc, ndc);
        max_ndc = max(max_ndc, ndc);
      }

      if (near_clipped) {
        min_ndc = float3(-1.0f);
        max_ndc = float3(1.0f, 1.0f, 1.0f);
      }

      float2 min_uv = clamp(min_ndc.xy * 0.5f + 0.5f, 0.0f, 1.0f);
      float2 max_uv = clamp(max_ndc.xy * 0.5f + 0.5f, 0.0f, 1.0f);

      float flipped_min_y = 1.0f - max_uv.y;
      float flipped_max_y = 1.0f - min_uv.y;
      min_uv.y = flipped_min_y;
      max_uv.y = flipped_max_y;

      float2 size_pixels =
          abs(max_uv - min_uv) * float2(pc.hiz_width, pc.hiz_height);
      float mip_count = float(hiz_mip_index(pc, 15u));
      float mip = clamp(ceil(log2(max(size_pixels.x, size_pixels.y))), 0.0f,
                        mip_count - 1.0f);

      uint target_view_idx = hiz_mip_index(pc, min(uint(mip), 14u));
      float2 center_uv = (min_uv + max_uv) * 0.5f;

      float4 depth_samples =
          sampled_images[NonUniformResourceIndex(target_view_idx)].GatherRed(
              samplers[NonUniformResourceIndex(pc.hiz_sampler_idx)], center_uv);

      float min_hiz_depth = min(min(depth_samples.x, depth_samples.y),
                                min(depth_samples.z, depth_samples.w));

      if (max_ndc.z < min_hiz_depth)
        visible = false;
    }
  }

  uint forward_base_cmd_id = pc.forward_instance_to_command[item_idx];
  if (forward_base_cmd_id == 0xFFFFFFFF)
    return;
  uint forward_lod_cmd_id = forward_base_cmd_id + cull.lod_idx;
  Ptr<VkDrawIndexedIndirectCommand, Access.ReadWrite, AddressSpace.Device>
      cmd_ptr = pc.forward_indirect_commands + forward_lod_cmd_id;
  if (visible && cmd_ptr != nullptr) {
    uint forward_slot;
    InterlockedAdd(cmd_ptr->instanceCount, 1, forward_slot);
    pc.forward_culled_remap[cmd_ptr->firstInstance + forward_slot] =
        item_idx | (cull.lod_idx << 28u);
  }
}

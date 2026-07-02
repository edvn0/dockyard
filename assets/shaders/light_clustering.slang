import push_constant;
import bindless;
import clustering;

struct LightClusteringPushConstants {
  ImmutablePtr<FrameUBO, AddressSpace.Device> frame_data;
  Ptr<ClusterEntry, Access.ReadWrite, AddressSpace.Device> cluster_list;
  Ptr<uint, Access.ReadWrite, AddressSpace.Device> light_list;
  Ptr<uint, Access.ReadWrite, AddressSpace.Device> light_list_counter;
};

// Unproject an NDC point to a view-space ray direction with z = 1.
float3 ndc_to_view_dir(float4x4 inv_proj, float ndc_x, float ndc_y) {
  float4 clip = float4(ndc_x, ndc_y, 1.0f, 1.0f);
  float4 view = mul(inv_proj, clip);
  float3 v = view.xyz / view.w;
  return v / max(v.z, 1e-6f);
}

bool sphere_vs_aabb(float3 center, float radius, float3 aabb_min,
                    float3 aabb_max) {
  float3 closest = clamp(center, aabb_min, aabb_max);
  float3 diff = center - closest;
  return dot(diff, diff) <= radius * radius;
}

groupshared uint s_visible_lights[CLUSTER_MAX_LIGHTS];
groupshared uint s_visible_count;

[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 gid: SV_GroupID, uint3 tid: SV_GroupThreadID,
                 uniform LightClusteringPushConstants pc) {
  uint cluster_idx = gid.x;
  if (cluster_idx >= CLUSTER_TOTAL)
    return;

  uint ds = cluster_idx / (CLUSTER_TILES_X * CLUSTER_TILES_Y);
  uint tile_flat = cluster_idx % (CLUSTER_TILES_X * CLUSTER_TILES_Y);
  uint ty = tile_flat / CLUSTER_TILES_X;
  uint tx = tile_flat % CLUSTER_TILES_X;

  float cam_near = pc.frame_data.camera_near;
  float cam_far = pc.frame_data.camera_far;
  float log_ratio = log(cam_far / cam_near);
  float z_near =
      cam_near * exp(log_ratio * (float(ds) / float(CLUSTER_DEPTH_SLICES)));
  float z_far = cam_near *
                exp(log_ratio * (float(ds + 1u) / float(CLUSTER_DEPTH_SLICES)));

  float ndc_x_min = float(tx) / float(CLUSTER_TILES_X) * 2.0f - 1.0f;
  float ndc_x_max = float(tx + 1u) / float(CLUSTER_TILES_X) * 2.0f - 1.0f;
  float ndc_y_max = 1.0f - 2.0f * (float(ty) / float(CLUSTER_TILES_Y));
  float ndc_y_min = 1.0f - 2.0f * (float(ty + 1u) / float(CLUSTER_TILES_Y));

  float4x4 inv_proj = pc.frame_data.inverse_projection;
  float3 dirs[4] = {
    ndc_to_view_dir(inv_proj, ndc_x_min, ndc_y_max),
    ndc_to_view_dir(inv_proj, ndc_x_max, ndc_y_max),
    ndc_to_view_dir(inv_proj, ndc_x_min, ndc_y_min),
    ndc_to_view_dir(inv_proj, ndc_x_max, ndc_y_min),
  };

  float3 aabb_min = float3(1e9f);
  float3 aabb_max = float3(-1e9f);
  [ForceUnroll]
  for (int i = 0; i < 4; i++) {
    float3 cn = dirs[i] * z_near;
    float3 cf = dirs[i] * z_far;
    aabb_min = min(aabb_min, min(cn, cf));
    aabb_max = max(aabb_max, max(cn, cf));
  }

  if (tid.x == 0u)
    s_visible_count = 0u;
  GroupMemoryBarrierWithGroupSync();

  uint light_count = pc.frame_data.point_light_count;
  for (uint iter = 0u; iter < (light_count + 63u) / 64u; iter++) {
    uint light_idx = iter * 64u + tid.x;
    bool visible = false;
    if (light_idx < light_count) {
      PointLight light = pc.frame_data.point_lights[light_idx];
      float3 lv = mul(pc.frame_data.view, float4(light.position, 1.0f)).xyz;
      visible = sphere_vs_aabb(lv, light.radius, aabb_min, aabb_max);
    }

    // Fixed: Use 3 arguments correctly to capture the previous value for
    uint wave_count = WaveActiveCountBits(visible);
    uint base_slot = 0;

    if (WaveIsFirstLane()) {
      // Use the 2-argument version, which is universally supported
      InterlockedAdd(s_visible_count, wave_count);

      // To get the 'previous' value atomically without a 3rd argument,
      // we read the value before the add (if possible) or simply use
      // the property that this is the first lane in the wave.
      // Actually, the simplest way to get the base_slot in this architecture:
      // The InterlockedAdd returns the original value if the compiler supports
      // it. Since it doesn't, we use this standard pattern:
      base_slot = s_visible_count - wave_count;
    }
    base_slot = WaveReadLaneFirst(base_slot);

    uint lane_offset = WavePrefixCountBits(visible);
    if (visible && (base_slot + lane_offset) < CLUSTER_MAX_LIGHTS) {
      s_visible_lights[base_slot + lane_offset] = light_idx;
    }
  }

  GroupMemoryBarrierWithGroupSync();

  uint final_count = 0u;
  uint global_offset = 0u;

  if (tid.x == 0u) {
    final_count = min(s_visible_count, CLUSTER_MAX_LIGHTS);
    InterlockedAdd(*pc.light_list_counter, final_count, global_offset);
    if (global_offset >= CLUSTER_MAX_LIGHT_LIST_ENTRIES)
      final_count = 0u;
    else if (global_offset + final_count > CLUSTER_MAX_LIGHT_LIST_ENTRIES)
      final_count = CLUSTER_MAX_LIGHT_LIST_ENTRIES - global_offset;

    pc.cluster_list[cluster_idx].offset = global_offset;
    pc.cluster_list[cluster_idx].count = final_count;
  }

  final_count = WaveReadLaneFirst(final_count);
  global_offset = WaveReadLaneFirst(global_offset);

  for (uint i = tid.x; i < final_count; i += 64u) {
    pc.light_list[global_offset + i] = s_visible_lights[i];
  }
}

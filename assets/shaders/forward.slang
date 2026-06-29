import push_constant;
import bindless;
import csm;
import pbr;
import clustering;

static const float PI = 3.14159265359f;

// OPTIMIZATION: Stripped down interpolators to bare minimum to save parameter
// cache space.
struct V2F {
  float4 pos : SV_Position;
  float3 world_pos : POSITION0;
  float3 normal : NORMAL;
  float4 tangent : TANGENT; // XYZ = Tangent, W = Bitangent Sign
  float2 uv : TEXCOORD0;
  uint material_id : TEXCOORD1;
  uint lod_idx : TEXCOORD2;
};

float4 sample_material_tex(uint idx, float2 uv) {
  return sampled_images[NonUniformResourceIndex(idx)].Sample(samplers[1], uv);
}

// Evaluate PBR contribution from all point lights visible to this cluster tile.
float3 evaluate_clustered_lights(
    Ptr<FrameUBO, Access.Read, AddressSpace.Device> frame_data,
    float4 sv_pos, // SV_Position of this fragment
    float3 world_pos, float3 n, float3 v,
    float ndotv_safe, float3 albedo_rgb, float roughness, float metallic,
    float3 f0, ImmutablePtr<ClusterEntry, AddressSpace.Device> cluster_list,
    ImmutablePtr<uint, AddressSpace.Device> light_list) {
  // Derive depth slice from view-space z (left-handed, z+ forward)
  float z_view = max(mul(frame_data.view, float4(world_pos, 1.0f)).z, frame_data.camera_near);
  float z_ratio = log(z_view / frame_data.camera_near) / log(frame_data.camera_far / frame_data.camera_near);
  uint depth_slice = min(uint(max(z_ratio * float(CLUSTER_DEPTH_SLICES), 0.0f)),
                         CLUSTER_DEPTH_SLICES - 1u);

  // Derive screen tile from SV_Position (0,0 = top-left)
  uint tile_x = min(uint(sv_pos.x / float(frame_data.viewport_width) * float(CLUSTER_TILES_X)),
                    CLUSTER_TILES_X - 1u);
  uint tile_y = min(uint(sv_pos.y / float(frame_data.viewport_height) * float(CLUSTER_TILES_Y)),
                    CLUSTER_TILES_Y - 1u);

  uint cluster_idx = depth_slice * (CLUSTER_TILES_X * CLUSTER_TILES_Y) +
                     tile_y * CLUSTER_TILES_X + tile_x;

  ClusterEntry cluster = cluster_list[cluster_idx];

  float3 result = float3(0.0f);
  for (uint ci = 0u; ci < cluster.count; ci++) {
    PointLight light = frame_data.point_lights[light_list[cluster.offset + ci]];

    float3 l = light.position - world_pos;
    float dist = length(l);
    if (dist >= light.radius)
      continue;
    l /= dist;

    float window = 1.0f - pow(saturate(dist / light.radius), 4.0f);
    float atten = (window * window) / max(dist * dist, 1e-4f);
    float3 radiance = light.color * light.intensity * atten;

    float3 h_l = normalize(v + l);
    float ndotl_l = max(dot(n, l), 0.0f);
    float3 f_l = fresnel_schlick(max(dot(h_l, v), 0.0f), f0);
    float ndf_l = distribution_ggx(n, h_l, roughness);
    float g_l = geometry_smith(n, v, l, roughness);
    float3 spec_l =
        (ndf_l * g_l * f_l) / max(4.0f * ndotv_safe * ndotl_l, 1e-4f);
    float3 kd_l = (1.0f - f_l) * (1.0f - metallic);

    result += (kd_l * albedo_rgb / PI + spec_l) * radiance * ndotl_l;
  }
  return result;
}

[shader("vertex")]
V2F vertexMain(uint vertex_id: SV_VulkanVertexID,
               uint instance_id: SV_InstanceID,
               uint inst_base: SV_VulkanInstanceID,
               uniform GpuPushConstants pc) {
  V2F output;

  let culled_remap_indices = pc.culled_index_remapping_buffer;
  let instance_data = pc.instance_buffer_ptr;

  uint packed_data = culled_remap_indices[NonUniformResourceIndex(inst_base)];
  uint global_id = packed_data & 0x0FFFFFFFu;
  var instance = unpack_instance_data(instance_data[global_id]);
  float4x4 model_matrix = instance.transform;
  float3x3 normal_matrix = (float3x3)model_matrix;

  Vertex v;
  if (instance.skinned_base != 0xFFFFFFFFu) {
    v = pc.skinned_vertex_buffer_ptr[instance.skinned_base + vertex_id -
                                     instance.source_vertex_offset];
  } else {
    v = pc.vertex_buffer_ptr[vertex_id];
  }

  output.lod_idx = packed_data >> 28u;

  float3 local_normal = unpackSnorm4x8ToFloat(v.normal).xyz;
  float4 local_tangent = unpackSnorm4x8ToFloat(v.tangent).xyzw;

  output.normal = normalize(mul(normal_matrix, local_normal));
  // Keep tangent sign in W, pass directly to fragment
  output.tangent = float4(normalize(mul(normal_matrix, local_tangent.xyz)),
                          -local_tangent.w);

  float4 world_pos = mul(
      model_matrix, float4(v.position[0], v.position[1], v.position[2], 1.0f));
  output.world_pos = world_pos.xyz;
  output.pos = mul(pc.frame_ubo.view_projection, world_pos);

  output.uv = unpackHalf2x16ToFloat(v.uvs);
  output.material_id = instance.material_id;

  return output;
}

[shader("fragment")]
float4 fragmentMain(V2F input, uniform GpuPushConstants pc) : SV_Target {
  GPUMaterial mat =
      pc.material_buffer_ptr[NonUniformResourceIndex(input.material_id)];

  float4 albedo =
      sample_material_tex(mat.albedo_index, input.uv) * mat.albedo_factor;

  [branch]
  if (mat.alpha_mode == 1u && albedo.a < mat.alpha_cutoff)
    discard;

  float3 geom_n = normalize(input.normal);
  float3 t =
      normalize(input.tangent.xyz - dot(input.tangent.xyz, geom_n) * geom_n);
  float3 b = cross(geom_n, t) * input.tangent.w;
  float3x3 tbn = float3x3(t, b, geom_n);

  float4 normal_sample = sample_material_tex(mat.normal_index, input.uv);
  float2 normal_xy = normal_sample.rg * 2.0f - 1.0f;
  normal_xy *= mat.normal_scale;

  float3 sampled_n;
  sampled_n.xy = normal_xy;
  sampled_n.z = sqrt(1.0f - dot(sampled_n.xy, sampled_n.xy));
  float3 n = normalize(mul(sampled_n, tbn));

  float4 mr = sample_material_tex(mat.metallic_roughness_index, input.uv);
  float roughness = clamp(mr.b * mat.roughness_factor, 0.0f, 1.0f);
  float metallic = clamp(mr.g * mat.metallic_factor, 0.04f, 0.1f);

  float3 emissive = sample_material_tex(mat.emissive_index, input.uv).rgb *
                    mat.emissive_factor.xyz * mat.emissive_factor.a;

  float ao = 1.0f;
  if ((mat.flags & MaterialFlags::combined_orm) != MaterialFlags::none) {
    ao = saturate(1.0f + mat.occlusion_strength * (mr.r - 1.0f));
  } else if ((mat.flags & MaterialFlags::no_occlusion) == MaterialFlags::none) {
    float occlusion = sample_material_tex(mat.occlusion_index, input.uv).r;
    ao = saturate(1.0f + mat.occlusion_strength * (occlusion - 1.0f));
  }

  float3 v = normalize(pc.frame_ubo.camera_pos.xyz - input.world_pos);
  float3 f0 = lerp(float3(0.04f), albedo.rgb, metallic);
  float ndotv_safe = max(dot(n, v), 1e-5f);

  const float3 sun_color = float3(1.0f, 0.95f, 0.8f);
  const float3 sun_dir = normalize(pc.frame_ubo.sun_direction.xyz);

  float3 h = normalize(v + sun_dir);
  float ndotl = max(dot(n, sun_dir), 0.0f);

  float horizon = saturate(1.0f + dot(geom_n, sun_dir));
  ndotl *= horizon * horizon;

  float3 f = fresnel_schlick(max(dot(h, v), 0.0f), f0);
  float ndf = distribution_ggx(n, h, roughness);
  float g = geometry_smith(n, v, sun_dir, roughness);

  float3 specular = (ndf * g * f) / max(4.0f * ndotv_safe * ndotl, 1e-4f);
  float3 kd = (1.0f - f) * (1.0f - metallic);
  float3 diffuse = kd * albedo.rgb / PI;

  float3 direct = (diffuse + specular) * sun_color * ndotl;

  float3 irradiance = sampled_cubemaps[NonUniformResourceIndex(
                                           pc.frame_ubo.ibl_irradiance_index)]
                          .Sample(samplers[pc.frame_ubo.ibl_sampler_index], n)
                          .rgb;

  float3 r = reflect(-v, n);
  float env_mip = roughness * float(pc.frame_ubo.ibl_prefiltered_mips - 1u);
  float3 prefiltered =
      sampled_cubemaps[NonUniformResourceIndex(
                           pc.frame_ubo.ibl_prefiltered_index)]
          .SampleLevel(samplers[pc.frame_ubo.ibl_sampler_index], r, env_mip)
          .rgb;

  float2 env_brdf =
      sampled_images[NonUniformResourceIndex(pc.frame_ubo.ibl_brdf_lut_index)]
          .Sample(samplers[0], float2(ndotv_safe, roughness))
          .rg;

  float3 f_ibl = fresnel_schlick_roughness(ndotv_safe, f0, roughness);
  float3 kd_ibl = (1.0f - f_ibl) * (1.0f - metallic);

  float3 diffuse_ibl = kd_ibl * albedo.rgb * irradiance;
  float3 specular_ibl = prefiltered * (f0 * env_brdf.r + env_brdf.g);
  float3 ambient = (diffuse_ibl + specular_ibl) * ao;

  var shadow_result =
      compute_shadow(input.world_pos, geom_n, sun_dir, *pc.frame_ubo);

  float3 point_light_contribution = evaluate_clustered_lights(
      pc.frame_ubo, input.pos,
      input.world_pos, n, v, ndotv_safe, albedo.rgb, roughness, metallic, f0,
      pc.cluster_list_ptr, pc.light_list_ptr);

  float3 final_color = (direct * shadow_result._0) + ambient + emissive +
                       point_light_contribution;

  // DEBUG: cluster light-count heatmap — comment out to restore normal
  // rendering
  /* {
    float z_view = max(mul(pc.frame_ubo.view, float4(input.world_pos, 1.0f)).z,
                       pc.frame_ubo.camera_near);
    float z_ratio = log(z_view / pc.frame_ubo.camera_near) /
                    log(pc.frame_ubo.camera_far / pc.frame_ubo.camera_near);
    uint depth_slice = min(uint(max(z_ratio * float(CLUSTER_DEPTH_SLICES),
  0.0f)), CLUSTER_DEPTH_SLICES - 1u); uint tile_x = min(uint(input.pos.x /
  float(pc.frame_ubo.viewport_width)  * float(CLUSTER_TILES_X)), CLUSTER_TILES_X
  - 1u); uint tile_y = min(uint(input.pos.y /
  float(pc.frame_ubo.viewport_height) * float(CLUSTER_TILES_Y)), CLUSTER_TILES_Y
  - 1u); uint cluster_idx = depth_slice * (CLUSTER_TILES_X * CLUSTER_TILES_Y) +
  tile_y * CLUSTER_TILES_X + tile_x; float t =
  saturate(float(pc.cluster_list_ptr[cluster_idx].count) / 8.0f); float3 heat =
  lerp(float3(0.0f, 0.0f, 0.2f), float3(1.0f, 0.0f, 0.0f), t); return
  float4(heat, 1.0f);
  } */

  return float4(final_color, albedo.a);
}

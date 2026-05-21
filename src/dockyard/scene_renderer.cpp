#include "PCH.hpp"
#include <dockyard/scene_renderer.hpp>

#include <atomic>
#include <dockyard/device_geometry.hpp>
#include <dockyard/mesh.hpp>
#include <execution>
#include <limits>
#include <ranges>

namespace dy {

namespace {

auto compute_cascade_splits(float near_z, float far_z, float lambda = 0.85f)
    -> std::array<float, shadow_map_cascade_count> {
  std::array<float, shadow_map_cascade_count> splits{};
  const float range = far_z - near_z;
  const float ratio = far_z / near_z;

  for (u32 i = 0; i < shadow_map_cascade_count; ++i) {
    const float p = static_cast<float>(i + 1) /
                    static_cast<float>(shadow_map_cascade_count);
    const float log_split = near_z * std::pow(ratio, p);
    const float uni_split = near_z + range * p;
    splits[i] = lambda * log_split + (1.0f - lambda) * uni_split;
  }
  return splits;
}

// Generic — works for both standard and reverse-Z projections.
// Reverse-Z: near_z -> 1.0, far_z -> 0.0.
auto split_to_ndc_z(const glm::mat4 &proj, float view_z) -> float {
  const glm::vec4 clip = proj * glm::vec4(0.0f, 0.0f, view_z, 1.0f);
  return clip.z / clip.w;
}

// z_near_ndc / z_far_ndc are in the camera's NDC convention.
// Reverse-Z: pass z_near_ndc=1.0, z_far_ndc<1.0 for each cascade.
auto frustum_corners_world(const glm::mat4 &inv_view_proj, float z_near_ndc,
                           float z_far_ndc) -> std::array<glm::vec3, 8> {
  const glm::vec4 ndc[8] = {
      {-1.0f, 1.0f, z_near_ndc, 1.0f}, {1.0f, 1.0f, z_near_ndc, 1.0f},
      {1.0f, -1.0f, z_near_ndc, 1.0f}, {-1.0f, -1.0f, z_near_ndc, 1.0f},
      {-1.0f, 1.0f, z_far_ndc, 1.0f},  {1.0f, 1.0f, z_far_ndc, 1.0f},
      {1.0f, -1.0f, z_far_ndc, 1.0f},  {-1.0f, -1.0f, z_far_ndc, 1.0f},
  };

  std::array<glm::vec3, 8> corners;
  for (u32 i = 0; i < 8; ++i) {
    const glm::vec4 world = inv_view_proj * ndc[i];
    const float w = std::abs(world.w) > 1e-5f ? world.w : 1.0f;
    corners[i] = glm::vec3(world) / w;
  }
  return corners;
}

// light_toward_sun: unit vector pointing *toward* the sun.
// This is the L vector used in dot(N, L) diffuse lighting — i.e. (0, 1, 0)
// for a sun directly overhead.
//
// Shadow maps use standard (non-reversed) Z.  Ortho projections do not have
// the precision cliffs that motivate reverse-Z, and standard Z keeps the
// sampler comparison op straightforward (VK_COMPARE_OP_LESS_OR_EQUAL).
auto compute_cascade(const glm::mat4 &camera_view, const glm::mat4 &camera_proj,
                     float prev_split_ndc, float curr_split_ndc,
                     float curr_split_view, const glm::vec3 &light_toward_sun)
    -> CascadeData {
  const glm::mat4 inv_view_proj = glm::inverse(camera_proj * camera_view);
  const auto corners =
      frustum_corners_world(inv_view_proj, prev_split_ndc, curr_split_ndc);

  // --- 1. Minimum bounding sphere ---
  glm::vec3 center(0.0f);
  for (const auto &c : corners)
    center += c;
  center /= 8.0f;

  float radius = 0.0f;
  for (const auto &c : corners)
    radius = std::max(radius, glm::distance(c, center));

  // Snap radius to texel grid to prevent cascade size shimmer.
  radius = std::ceil(radius * 16.0f) / 16.0f;

  // --- 2. Light-view matrix ---
  // Eye is placed behind the scene *in the direction of the sun*, so the
  // camera looks from the sun's side toward the scene (correct orientation).
  //
  // z_extent is how far the eye is from center in light-view space.
  // The 500-unit margin buys depth for shadow casters behind the view frustum.
  const glm::vec3 up = std::abs(light_toward_sun.y) > 0.99f
                           ? glm::vec3(0.0f, 0.0f, 1.0f)
                           : glm::vec3(0.0f, 1.0f, 0.0f);
  const float z_extent = radius + 500.0f;
  const glm::vec3 eye = center + light_toward_sun * z_extent;
  glm::mat4 light_view = glm::lookAtLH(eye, center, up);

  // --- 3. Texel snapping ---
  // Transform the world origin into light-view "texel" space, round it, and
  // apply the residual as a translation correction.  This keeps the shadow
  // texel grid stationary as the camera moves, eliminating edge crawl.
  const float texels_per_unit =
      static_cast<float>(shadow_map_cascade_resolution) / (radius * 2.0f);
  glm::vec4 shadow_origin = light_view * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
  shadow_origin *= texels_per_unit;

  const glm::vec4 rounded = glm::round(shadow_origin);
  glm::vec4 offset = (rounded - shadow_origin) / texels_per_unit;
  offset.z = 0.0f; // never shift depth — that would break near/far
  offset.w = 0.0f;
  light_view[3] += offset;

  // --- 4. Orthographic projection ---
  // In light-view space the eye is at the origin.  The bounding sphere center
  // is at Z = z_extent (along +Z, since lookAtLH makes +Z point toward center).
  // The sphere occupies [z_extent - radius, z_extent + radius] on the Z axis.
  //
  // We extend near/far by caster_margin to capture shadow casters that lie
  // outside the camera frustum but still cast into it.
  const float caster_margin = 500.0f;
  const float ortho_near = z_extent - radius - caster_margin;
  const float ortho_far = z_extent + radius + caster_margin;

  const glm::mat4 light_proj = glm::orthoLH_ZO(-radius, radius, // left, right
                                               -radius, radius, // bottom, top
                                               ortho_near, ortho_far);

  return {
      .view_proj = light_proj * light_view,
      .split_depth = curr_split_view,
  };
}

} // namespace

struct PaddedDrawCommand {
  u32 index_count;
  u32 instance_count;
  u32 first_index;
  i32 vertex_offset;
  u32 first_instance;
};

auto create_main_pipeline_layout(VkDevice device,
                                 VkDescriptorSetLayout bindless_layout)
    -> VkPipelineLayout {
  const VkPushConstantRange push_range{
      .stageFlags = VK_SHADER_STAGE_ALL,
      .offset = 0U,
      .size = sizeof(GpuPushConstants),
  };

  VkPipelineLayoutCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  ci.setLayoutCount = bindless_layout != VK_NULL_HANDLE ? 1u : 0u;
  ci.pSetLayouts = &bindless_layout, ci.pushConstantRangeCount = 1u;
  ci.pPushConstantRanges = &push_range;

  VkPipelineLayout layout = VK_NULL_HANDLE;
  vkCreatePipelineLayout(device, &ci, nullptr, &layout);
  return layout;
}

auto create_composite_pipeline(VkDevice device,
                               VkDescriptorSetLayout bindless_layout,
                               VkPipelineLayout &out_layout,
                               VkPipeline &out_pipeline) -> void {
  const VkPushConstantRange push_range{
      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
      .offset = 0U,
      .size = sizeof(CompositePushConstants),
  };
  const VkPipelineLayoutCreateInfo layout_ci{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1U,
      .pSetLayouts = &bindless_layout,
      .pushConstantRangeCount = 1U,
      .pPushConstantRanges = &push_range,
  };
  vkCreatePipelineLayout(device, &layout_ci, nullptr, &out_layout);

  auto result = build_graphics_pipeline(
      device,
      GraphicsPipelineDescription{
          .shader_path = VFSPath::create("shaders://composite.slang"),
          .layout = out_layout,
          .render_targets = {.color_formats = {VK_FORMAT_R8G8B8A8_UNORM}},
          .cull_mode = VK_CULL_MODE_NONE,
          .blending = {BlendMode::opaque()},
      });
  if (!result) {
    error("composite pipeline: {}", result.error());
    std::abort();
  }
  out_pipeline = *result;
}

auto create_culling_pipeline(VkDevice device,
                             VkDescriptorSetLayout bindless_layout,
                             VkPipelineLayout &out_layout,
                             VkPipeline &out_pipeline) -> void {
  const VkPushConstantRange push_range{
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      .offset = 0U,
      .size = sizeof(CullingPushConstants),
  };
  const VkPipelineLayoutCreateInfo layout_ci{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1U,
      .pSetLayouts = &bindless_layout,
      .pushConstantRangeCount = 1U,
      .pPushConstantRanges = &push_range,
  };
  vkCreatePipelineLayout(device, &layout_ci, nullptr, &out_layout);

  auto result = build_compute_pipeline(
      device, ComputePipelineDescription{
                  .shader_path = VFSPath::create("shaders://culling.slang"),
                  .layout = out_layout,
              });
  if (!result) {
    error("culling pipeline: {}", result.error());
    std::abort();
  }
  out_pipeline = *result;
}

auto SceneRenderer::register_gltf(MeshAsset &&asset) -> MeshHandle {
  u32 id = static_cast<u32>(mesh_registry.size());
  mesh_registry.push_back(std::move(asset));
  return MeshHandle{id};
}
auto SceneRenderer::register_external_view(VkImageView view,
                                           VkImageViewType type)
    -> TextureHandle {
  TextureEntry entry{
      .texture =
          {
              .sampled_view = view,
              .owned = false,
          },
      .sampled_view_type = type,
  };
  TextureHandle handle = textures.create(std::move(entry));

  bindless.queue_texture_write(handle.index(), view, VK_NULL_HANDLE, type);
  bindless.need_repopulate = true;
  return handle;
}

auto SceneRenderer::get_mesh(MeshHandle handle) -> MeshAsset * {
  if (!handle.valid() || handle.index() >= mesh_registry.size())
    return nullptr;
  return &mesh_registry[handle.index()];
}
auto SceneRenderer::get_mesh(MeshHandle handle) const -> const MeshAsset * {
  if (!handle.valid() || handle.index() >= mesh_registry.size())
    return nullptr;
  return &mesh_registry[handle.index()];
}

auto SceneRenderer::get_material_view(MeshHandle handle) const
    -> ConstMaterialView {
  const auto *mesh = get_mesh(handle);
  assert(mesh && "invalid mesh handle");
  return get_material_view(*mesh);
}

auto SceneRenderer::get_material_view_mut(MeshHandle handle)
    -> MutableMaterialView {
  const auto *mesh = get_mesh(handle);
  assert(mesh && "invalid mesh handle");
  return get_material_view_mut(*mesh);
}

auto SceneRenderer::get_material_view(const MeshAsset &mesh) const
    -> ConstMaterialView {
  return {
      .materials = geometry_pool->get_materials(mesh.material_base_slot,
                                                mesh.material_count),
      .base_slot = mesh.material_base_slot,
  };
}

auto SceneRenderer::get_material_view_mut(const MeshAsset &mesh) const
    -> MutableMaterialView {
  return {
      .materials = geometry_pool->get_materials_mut(mesh.material_base_slot,
                                                    mesh.material_count),
      .base_slot = mesh.material_base_slot,
  };
}

SceneRenderer::SceneRenderer(VulkanContext &c, SwapchainResources &sc)
    : ctx(c), swapchain(sc),
      depth_prepass(RenderPassType::DepthPrepass, ctx.allocator),
      forward_pass(RenderPassType::Forward, ctx.allocator) {
  constexpr auto vertex_count = 1'000'000;
  constexpr auto index_count = 10'000'000;
  constexpr auto material_count = 500;
  geometry_pool = GeometryPool::create(
      ctx.allocator, vertex_count * sizeof(Vertex),
      vertex_count * sizeof(PositionOnlyVertex), index_count * sizeof(u32),
      material_count * sizeof(GPUMaterial));

  const u32 white = 0xFFFFFFFF;
  const u32 blue = glm::packUnorm4x8(glm::vec4(0.5f, 0.5f, 1.0f, 1.0f));
  const u32 mr_default = glm::packUnorm4x8(glm::vec4(0.0f, 1.0f, 0.0f, 1.0f));
  const u32 occlusion_default = 0xFFFFFFFF;
  const u32 black = 0xFF000000;
  white_texture = upload_texture(std::span(&white, 1), "white_fallback_texture",
                                 1, 1, VK_FORMAT_R8G8B8A8_UNORM, false);
  normal_texture =
      upload_texture(std::span(&blue, 1), "normal_fallback_texture", 1, 1,
                     VK_FORMAT_R8G8B8A8_UNORM, false);
  metallic_roughness_texture = upload_texture(
      std::span(&mr_default, 1), "metallic_roughness_fallback_texture", 1, 1,
      VK_FORMAT_R8G8B8A8_UNORM, false);
  occlusion_texture = upload_texture(std::span(&occlusion_default, 1),
                                     "occlusion_fallback_texture", 1, 1,
                                     VK_FORMAT_R8G8B8A8_UNORM, false);
  black_texture = upload_texture(std::span(&black, 1), "black_fallback_texture",
                                 1, 1, VK_FORMAT_R8G8B8A8_UNORM, false);
  assert(white_texture.index() == 0);
  info("White texture index: {}", white_texture.index());
  dummy_texture_handle = white_texture;

  resize();
}
auto SceneRenderer::initialise_bindless() -> void {

  const VkSamplerCreateInfo sampler_ci{
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_LINEAR,
      .minFilter = VK_FILTER_LINEAR,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
      .mipLodBias = 0.0f,
      .anisotropyEnable = VK_TRUE,
      .maxAnisotropy = 16,
      .compareEnable = VK_FALSE,
      .minLod = 0.0f,
      .maxLod = 1.0f,
      .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
      .unnormalizedCoordinates = VK_FALSE,
  };
  vk::check(
      vkCreateSampler(ctx.device, &sampler_ci, nullptr, &dummy_sampler_vk));
  dummy_sampler_handle =
      samplers.create(SamplerEntry{.sampler = dummy_sampler_vk});

  const VkSamplerCreateInfo comparison_ci{
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_LINEAR,
      .minFilter = VK_FILTER_LINEAR,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .compareEnable = VK_TRUE,
      .compareOp = VK_COMPARE_OP_GREATER_OR_EQUAL,
      .minLod = 0.0f,
      .maxLod = 1.0f,
      .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,
  };
  vk::check(vkCreateSampler(ctx.device, &comparison_ci, nullptr,
                            &comparison_sampler_vk));
  comparison_samplers.create(SamplerEntry{.sampler = comparison_sampler_vk});

  const BindlessCaps caps = query_bindless_caps(ctx.physical_device);
  bindless.init(ctx.device, caps,
                /*initial_textures            =*/1024u,
                /*initial_samplers            =*/64u,
                /*initial_comparison_samplers =*/8u,
                /*initial_storage_images      =*/512u,
                /*initial_accel_structs       =*/0u);

  pipeline_layout = create_main_pipeline_layout(ctx.device, bindless.layout);
  pipeline_registry = std::make_unique<PipelineRegistry>(ctx.device);

  create_composite_pipeline(ctx.device, bindless.layout,
                            composite_pipeline_layout, composite_pipeline);
  DeletionQueue::the().destroy_at_exit(ctx.device, composite_pipeline,
                                       composite_pipeline_layout);

  create_culling_pipeline(ctx.device, bindless.layout, culling_pipeline_layout,
                          culling_pipeline);
  DeletionQueue::the().destroy_at_exit(ctx.device, culling_pipeline,
                                       culling_pipeline_layout);

  const VkSamplerCreateInfo shadow_ci{
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_LINEAR,
      .minFilter = VK_FILTER_LINEAR,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
      .compareEnable = VK_TRUE,
      .compareOp = VK_COMPARE_OP_LESS_OR_EQUAL, // shadow maps are conventional
      .minLod = 0.0f,
      .maxLod = 1.0f,
      .borderColor =
          VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE, // 1.0 = fully lit outside
  };
  vk::check(vkCreateSampler(ctx.device, &shadow_ci, nullptr,
                            &shadow_comparison_sampler_vk));
  shadow_sampler_bindless_idx =
      comparison_samplers
          .create(SamplerEntry{.sampler = shadow_comparison_sampler_vk})
          .index();

  init_csm();

  {
    auto result = pipeline_registry->create_graphics({
        .shader_path = VFSPath::create("shaders://shadow.slang"),
        .layout = pipeline_layout,
        .render_targets = {.depth_format = VK_FORMAT_D32_SFLOAT},
        .cull_mode = VK_CULL_MODE_BACK_BIT,
        .samples = VK_SAMPLE_COUNT_1_BIT, // no MSAA on shadow maps
        .depth = {.test = true,
                  .write = true,
                  .compare_op = VK_COMPARE_OP_LESS_OR_EQUAL}, // conventional
        .extra_dynamic_states =
            {
                VK_DYNAMIC_STATE_CULL_MODE,
                VK_DYNAMIC_STATE_DEPTH_BIAS,
            },
    });
    if (!result) {
      error("shadow pipeline: {}", result.error());
      std::abort();
    }
    shadow_pipeline = *result;
  }
}
auto SceneRenderer::upload_texture(std::span<const u32> data,
                                   std::string_view name, u32 w, u32 h,
                                   VkFormat fmt, bool gen_mips, bool storage)
    -> TextureHandle {
  auto tex = Texture::from_bytes(ctx, name,
                                 {
                                     .bytes = data,
                                     .width = w,
                                     .height = h,
                                     .format = fmt,
                                     .generate_mips = gen_mips,
                                     .storage_view = storage,
                                 });

  bindless.need_repopulate = true;
  return textures.create(TextureEntry{
      .texture = std::move(tex),
      .sampled_view_type = VK_IMAGE_VIEW_TYPE_2D,
  });
}
auto SceneRenderer::resize() -> void {
  frame_ubo_buffers.clear();
  for (u32 i = 0U; i < frames_in_flight; ++i) {
    frame_ubo_buffers.emplace_back(Buffer::create(
        ctx.allocator, sizeof(FrameUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT));
  }
  bindless.need_repopulate = true;
}
auto SceneRenderer::destroy() -> void {
  csm.destroy(ctx.device, ctx.allocator);

  bindless.destroy();

  std::ranges::for_each(textures.mutable_data(), [&c = ctx](auto &v) {
    auto &&[tex, view] = v.object;
    tex.destroy(c);
  });
  std::ranges::for_each(samplers.mutable_data(), [&c = ctx](auto &v) {
    auto &&[sampler] = v.object;
    DeletionQueue::the().push(
        [dev = c.device, s = sampler] { vkDestroySampler(dev, s, nullptr); });
  });
  std::ranges::for_each(
      comparison_samplers.mutable_data(), [&c = ctx](auto &v) {
        auto &&[sampler] = v.object;
        DeletionQueue::the().push([dev = c.device, s = sampler] {
          vkDestroySampler(dev, s, nullptr);
        });
      });

  if (pipeline_registry)
    pipeline_registry->cleanup();
  if (pipeline_layout != VK_NULL_HANDLE)
    vkDestroyPipelineLayout(ctx.device, pipeline_layout, nullptr);
}

void SceneRenderer::submit(MeshHandle handle, const glm::mat4 &t,
                           u32 pipeline_id, u32 material_id) {
  auto *asset = get_mesh(handle);
  if (asset == nullptr)
    return;

  for (const auto &node : asset->nodes) {
    glm::mat4 node_transform = t * node.local_transform;
    for (const auto &prim : node.primitives) {
      u32 id = static_cast<u32>(submission_queue.size());
      u32 resolved_mat = material_id != 0 ? material_id : prim.material_id;

      submission_queue.emplace_back(PendingDraw{
          .mesh = prim.mesh,
          .pipeline_id = pipeline_id,
          .material_id = resolved_mat,
          .transform = node_transform,
          .aabb = prim.aabb,
          .instance_id = id,
      });

      // const auto &mat =
      // get_material_view(*asset).materials[prim.material_id];

      // each pass decides membership here
      if (true) {
        u64 depth_key = static_cast<u64>(prim.mesh.first_index);
        auto &b = depth_prepass.buckets[depth_key];
        b.mesh = prim.mesh;
        b.pipeline_id = pipeline_id;
        b.instance_ids.push_back(id);
      }

      {
        u64 fwd_key = (static_cast<u64>(pipeline_id) << 48) |
                      (static_cast<u64>(resolved_mat) << 32) |
                      prim.mesh.first_index;
        auto &b = forward_pass.buckets[fwd_key];
        b.mesh = prim.mesh;
        b.pipeline_id = pipeline_id;
        b.material_id = resolved_mat;
        b.instance_ids.push_back(id);
      }
    }
  }
}

void SceneRenderer::ensure_global_capacity(usize instance_count) {
  if (const auto size = instance_count * sizeof(InstanceData);
      !global_instance_buffer || global_instance_buffer->size() < size) {
    global_instance_buffer =
        Buffer::create(ctx.allocator, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  }
}

namespace {
std::atomic_uint64_t current_frame_index{std::numeric_limits<u64>::max()};

auto extract_frustum_planes(const glm::mat4 &vp)
    -> const std::array<glm::vec4, 6> & {
  static glm::mat4 last_vp{0.0F};
  static std::array<glm::vec4, 6> cached_planes{};

  if (vp == last_vp) {
    return cached_planes;
  }
  last_vp = vp;

  const auto row = [&](const int i) -> glm::vec4 {
    return {vp[0][i], vp[1][i], vp[2][i], vp[3][i]};
  };
  const glm::vec4 r0 = row(0);
  const glm::vec4 r1 = row(1);
  const glm::vec4 r2 = row(2);
  const glm::vec4 r3 = row(3);

  cached_planes = {
      r3 + r0, r3 - r0, r3 - r1, r3 + r1, r2, r3 - r2,
  };

  for (auto &p : cached_planes) {
    p /= glm::length(glm::vec3(p));
  }

  return cached_planes;
}
}

auto SceneRenderer::prepare(u64 frame_index, const glm::mat4 &view,
                            const glm::mat4 &projection) -> bool {
  current_frame_index = frame_index;
  if (submission_queue.empty())
    return false;

  ensure_global_capacity(submission_queue.size());
  global_instance_data.clear();

  for (const auto &draw : submission_queue) {
    glm::vec3 half_extents = (draw.aabb.get_max() - draw.aabb.get_min()) * 0.5F;
    float radius = glm::length(half_extents);
    global_instance_data.emplace_back(draw.transform, draw.material_id, radius);
  }

  global_instance_buffer->upload(global_instance_data);

  const glm::mat4 inv_view = glm::inverse(view);
  const glm::mat4 inv_proj = glm::inverse(projection);
  const glm::mat4 view_proj = projection * view;
  const FrameUBO ubo{
      .view = view,
      .projection = projection,
      .view_projection = view_proj,
      .inverse_projection = inv_proj,
      .inverse_view = inv_view,
      .inverse_view_projection = glm::inverse(view_proj),
      .camera_position = inv_view[3],
      // Just copy the struct directly — one memcpy worth of data
      .cascades = csm_frame_data.cascades,
      .frustum_planes = extract_frustum_planes(view_proj),
      .shadow_array_index = csm_frame_data.shadow_array_index,
      .shadow_sampler_index = csm_frame_data.shadow_sampler_index,
  };

  frame_ubo_buffers.at(frame_index)->upload(std::span(&ubo, 1));
  if (!depth_prepass.bake(global_instance_data.size())) {
    return false;
  }
  if (!forward_pass.bake(global_instance_data.size())) {
    return false;
  }

  for (auto &v : depth_prepass.buckets | std::views::values) {
    v.instance_ids.clear();
  }
  for (auto &v : forward_pass.buckets | std::views::values) {
    v.instance_ids.clear();
  }
  submission_queue.clear();
  return true;
}

void SceneRenderer::render_pass(VkCommandBuffer cmd, RenderPass &pass,
                                VkPipeline override_pipeline) {
  auto &pool = *geometry_pool;
  if (pass.batches.empty())
    return;

  auto &ws = pass.frame_workspaces.at(current_frame_index);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout,
                          0U, 1U, &bindless.set, 0U, nullptr);

  const GpuPushConstants push_constants{
      .vertex_buffer_ptr =
          DeviceAddress{
              pool.vertex_buffer->get_device_address(),
          },
      .position_only_buffer_ptr =
          DeviceAddress{
              pool.position_only_vertex_buffer->get_device_address(),
          },
      .transform_buffer_ptr =
          DeviceAddress{
              global_instance_buffer->get_device_address(),
          },
      .culled_index_remapping_buffer =
          DeviceAddress{
              ws.culled_index_remapping_buffer->get_device_address(),
          },
      .frame_ubo =
          DeviceAddress{
              frame_ubo_buffers.at(current_frame_index)->get_device_address(),
          },
      .material_ptr =
          DeviceAddress{
              pool.material_buffer->get_device_address(),
          },
  };

  for (const auto &batch : pass.batches) {
    const VkPipeline pipe = override_pipeline != VK_NULL_HANDLE
                                ? override_pipeline
                                : pipeline_registry->get(batch.pipeline_id);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_ALL, 0u,
                       sizeof(GpuPushConstants), &push_constants);
    vkCmdDrawIndexedIndirectCount(
        cmd, ws.indirect_buffer->get_buffer(),
        batch.first_command_index * sizeof(PaddedDrawCommand),
        ws.count_buffer->get_buffer(), batch.count_buffer_offset,
        batch.max_command_count, sizeof(PaddedDrawCommand));
  }
}

void SceneRenderer::composite_pass(VkCommandBuffer cmd) {
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, composite_pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          composite_pipeline_layout, 0u, 1u, &bindless.set, 0u,
                          nullptr);
  const CompositePushConstants push{
      .forward_texture_index = forward_target_handle,
      .sampler = dummy_sampler_handle,
  };
  vkCmdPushConstants(cmd, composite_pipeline_layout,
                     VK_SHADER_STAGE_FRAGMENT_BIT, 0u, sizeof(push), &push);
  vkCmdDraw(cmd, 3U, 1u, 0u, 0u);
}

void SceneRenderer::culling_pass(VkCommandBuffer cmd) {
  auto geometry_count = global_instance_data.size();
  if (geometry_count == 0 || depth_prepass.batches.empty() ||
      forward_pass.batches.empty())
    return;
  auto &depth_ws = depth_prepass.frame_workspaces.at(current_frame_index);
  auto &forward_ws = forward_pass.frame_workspaces.at(current_frame_index);
  {
    static constexpr u32 zero_value = 0U;
    u32 depth_cmd_count = static_cast<u32>(depth_ws.indirect_buffer->size() /
                                           sizeof(PaddedDrawCommand));
    for (u32 i = 0; i < depth_cmd_count; ++i) {
      VkDeviceSize offset = (i * sizeof(PaddedDrawCommand)) +
                            offsetof(PaddedDrawCommand, instance_count);
      vkCmdUpdateBuffer(cmd, depth_ws.indirect_buffer->get_buffer(), offset,
                        sizeof(u32), &zero_value);
    }

    u32 forward_cmd_count = static_cast<u32>(
        forward_ws.indirect_buffer->size() / sizeof(PaddedDrawCommand));
    for (u32 i = 0; i < forward_cmd_count; ++i) {
      VkDeviceSize offset = (i * sizeof(PaddedDrawCommand)) +
                            offsetof(PaddedDrawCommand, instance_count);
      vkCmdUpdateBuffer(cmd, forward_ws.indirect_buffer->get_buffer(), offset,
                        sizeof(u32), &zero_value);
    }
  }

  VkBufferMemoryBarrier2 clear_barriers[2] = {
      {
          .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
          .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
          .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
          .dstAccessMask =
              VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
          .buffer = depth_ws.indirect_buffer->get_buffer(),
          .size = VK_WHOLE_SIZE,
      },
      {
          .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
          .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
          .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
          .dstAccessMask =
              VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
          .buffer = forward_ws.indirect_buffer->get_buffer(),
          .size = VK_WHOLE_SIZE,
      },
  };

  VkDependencyInfo clear_dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                             .bufferMemoryBarrierCount = 2U,
                             .pBufferMemoryBarriers = clear_barriers};
  vkCmdPipelineBarrier2(cmd, &clear_dep);

  CullingPushConstants push{
      .instance_buffer = global_instance_buffer->get_device_address(),
      .frame_data =
          frame_ubo_buffers.at(current_frame_index)->get_device_address(),

      .depth_original_remap_buffer =
          depth_ws.index_remapping_buffer->get_device_address(),
      .depth_instance_to_command_buffer =
          depth_ws.instance_to_command_buffer->get_device_address(),
      .depth_indirect_commands = depth_ws.indirect_buffer->get_device_address(),
      .depth_culled_remap =
          depth_ws.culled_index_remapping_buffer->get_device_address(),

      .forward_original_remap_buffer =
          forward_ws.index_remapping_buffer->get_device_address(),
      .forward_instance_to_command_buffer =
          forward_ws.instance_to_command_buffer->get_device_address(),
      .forward_indirect_commands =
          forward_ws.indirect_buffer->get_device_address(),
      .forward_culled_remap =
          forward_ws.culled_index_remapping_buffer->get_device_address(),
      .total_instance_count = static_cast<u32>(geometry_count),
  };

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, culling_pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                          culling_pipeline_layout, 0U, 1U, &bindless.set, 0U,
                          nullptr);
  vkCmdPushConstants(cmd, culling_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                     0U, sizeof(push), &push);

  auto dispatch_count = (geometry_count + 63) / 64;
  vkCmdDispatch(cmd, dispatch_count, 1, 1);

  VkBufferMemoryBarrier2 post_cull_barriers[4] = {
      // Indirect Commands
      {
          .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
          .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
          .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
          .dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
          .buffer = depth_ws.indirect_buffer->get_buffer(),
          .size = VK_WHOLE_SIZE,
      },
      {
          .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
          .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
          .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
          .dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
          .buffer = forward_ws.indirect_buffer->get_buffer(),
          .size = VK_WHOLE_SIZE,
      },
      // Culled Remapping Arrays
      {
          .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
          .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
          .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
          .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
          .buffer = depth_ws.culled_index_remapping_buffer->get_buffer(),
          .size = VK_WHOLE_SIZE,
      },
      {
          .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
          .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
          .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
          .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
          .buffer = forward_ws.culled_index_remapping_buffer->get_buffer(),
          .size = VK_WHOLE_SIZE,
      },
  };

  VkDependencyInfo post_dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                            .bufferMemoryBarrierCount = 4U,
                            .pBufferMemoryBarriers = post_cull_barriers};
  vkCmdPipelineBarrier2(cmd, &post_dep);
}
auto RenderPass::ensure_capacity(usize command_count, usize instance_count,
                                 usize batch_count,
                                 usize total_global_instances) -> bool {
  constexpr VkBufferUsageFlags indirect_flags =
      VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  constexpr VkBufferUsageFlags storage_flags =
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

  usize indirect_needed = command_count * sizeof(PaddedDrawCommand);
  usize count_needed = batch_count * sizeof(u32);
  usize global_needed = total_global_instances * sizeof(u32);
  usize instance_needed = instance_count * sizeof(u32);

  VmaAllocatorInfo allocator_info{};
  vmaGetAllocatorInfo(allocator, &allocator_info);

  auto &ws = frame_workspaces.at(current_frame_index);

  auto ensure_buffer = [alloc = allocator, dev = allocator_info.device](
                           std::unique_ptr<Buffer> &buffer, usize needed_size,
                           VkBufferUsageFlags flags) {
    if (!buffer || buffer->size() < needed_size) {
      if (buffer) {
        vkDeviceWaitIdle(dev);
      }
      buffer = Buffer::create(alloc, needed_size, flags);
    }
  };

  ensure_buffer(ws.indirect_buffer, indirect_needed,
                indirect_flags | VK_BUFFER_USAGE_2_TRANSFER_DST_BIT);
  ensure_buffer(ws.count_buffer, count_needed, indirect_flags);
  ensure_buffer(ws.instance_to_command_buffer, global_needed, storage_flags);
  ensure_buffer(ws.index_remapping_buffer, instance_needed, storage_flags);
  ensure_buffer(ws.culled_index_remapping_buffer, instance_needed,
                storage_flags);

  return true;
}

auto RenderPass::bake(usize total_global_instances) -> bool {
  std::vector<PaddedDrawCommand> commands;
  std::vector<u32> remapped_indices;
  std::vector<u32> draw_counts;
  std::vector<u32> instance_to_commands(total_global_instances, 0xFFFFFFFF);

  batches.clear();
  u32 current_pipeline = ~0U;

  for (auto &&[key, bucket] : buckets) {
    if (bucket.pipeline_id != current_pipeline) {
      current_pipeline = bucket.pipeline_id;
      draw_counts.push_back(0U);
      batches.push_back({
          .pipeline_id = bucket.pipeline_id,
          .max_command_count = 0U,
          .first_command_index = static_cast<u32>(commands.size()),
          .count_buffer_offset =
              static_cast<u32>((draw_counts.size() - 1) * sizeof(u32)),
      });
    }

    const u32 first_instance = static_cast<u32>(remapped_indices.size());
    const u32 cmd_idx = static_cast<u32>(commands.size());

    commands.push_back({
        .index_count = bucket.mesh.index_count,
        .instance_count = 0U,
        .first_index = bucket.mesh.first_index,
        .vertex_offset = bucket.mesh.vertex_offset,
        .first_instance = first_instance,
    });
    batches.back().max_command_count++;
    draw_counts.back()++;

    for (u32 iid : bucket.instance_ids) {
      remapped_indices.push_back(iid);
      instance_to_commands[iid] = cmd_idx;
    }

    bucket.instance_ids.clear();
  }

  ensure_capacity(commands.size(), remapped_indices.size(), draw_counts.size(),
                  total_global_instances);

  auto &ws = frame_workspaces.at(current_frame_index);
  ws.indirect_buffer->upload_with_offset(commands, 0);
  ws.count_buffer->upload_with_offset(draw_counts, 0);
  ws.index_remapping_buffer->upload_with_offset(remapped_indices, 0);
  ws.instance_to_command_buffer->upload_with_offset(instance_to_commands, 0);

  return true;
}

[[nodiscard]] constexpr auto PendingDraw::get_key(RenderPassType pass) const
    -> u64 {
  const u32 mesh_id = mesh.first_index;
  if (pass == RenderPassType::DepthPrepass)
    return static_cast<u64>(mesh_id);
  return (static_cast<u64>(pipeline_id) << 48) |
         (static_cast<u64>(material_id) << 32) | mesh_id;
}

void SceneRenderer::render_shadow_cascade(VkCommandBuffer cmd,
                                          u32 cascade_idx) {
  auto &pass = depth_prepass;
  const auto &pipeline = pipeline_registry->get(shadow_pipeline);
  if (pass.batches.empty())
    return;

  auto &ws = pass.frame_workspaces.at(current_frame_index);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout,
                          0U, 1u, &bindless.set, 0u, nullptr);
  vkCmdBindIndexBuffer(cmd, geometry_pool->index_buffer->get_buffer(), 0u,
                       VK_INDEX_TYPE_UINT32);

  auto &pool = *geometry_pool;
  const GpuPushConstants push_constants{
      .vertex_buffer_ptr =
          DeviceAddress{
              pool.vertex_buffer->get_device_address(),
          },
      .position_only_buffer_ptr =
          DeviceAddress{
              pool.position_only_vertex_buffer->get_device_address(),
          },
      .transform_buffer_ptr =
          DeviceAddress{
              global_instance_buffer->get_device_address(),
          },
      .culled_index_remapping_buffer =
          DeviceAddress{
              ws.culled_index_remapping_buffer->get_device_address(),
          },
      .frame_ubo =
          DeviceAddress{
              frame_ubo_buffers.at(current_frame_index)->get_device_address(),
          },
      .material_ptr =
          DeviceAddress{
              pool.material_buffer->get_device_address(),
          },
      .cascade_index = cascade_idx,
  };

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_ALL, 0U,
                     sizeof(GpuPushConstants), &push_constants);

  for (const auto &batch : pass.batches) {
    vkCmdDrawIndexedIndirectCount(
        cmd, ws.indirect_buffer->get_buffer(),
        batch.first_command_index * sizeof(PaddedDrawCommand),
        ws.count_buffer->get_buffer(), batch.count_buffer_offset,
        batch.max_command_count, sizeof(PaddedDrawCommand));
  }
}

void SceneRenderer::update_csm(const glm::mat4 &view, const glm::mat4 &proj,
                               float camera_near, float camera_far) {
  const auto splits = compute_cascade_splits(camera_near, camera_far);

  // Reverse-Z: the camera near plane sits at NDC Z = 1.0.
  // We walk outward cascade by cascade; each split's NDC is < the previous.
  float prev_ndc = 1.0f;

  for (u32 i = 0u; i < shadow_map_cascade_count; ++i) {
    const float view_z = splits[i];
    const float curr_ndc = split_to_ndc_z(proj, view_z);

    csm_frame_data.cascades[i] = compute_cascade(
        view, proj, prev_ndc, curr_ndc, view_z,
        sun_direction); // sun_direction == toward-sun (L vector)

    prev_ndc = curr_ndc;
  }

  csm_frame_data.shadow_array_index = csm.bindless_handle.index();
  csm_frame_data.shadow_sampler_index = shadow_sampler_bindless_idx;
}

auto CsmResources::destroy(VkDevice device, VmaAllocator allocator) -> void {
  vmaDestroyImage(allocator, image, allocation);
  for (auto &v : layer_views)
    vkDestroyImageView(device, v, nullptr);
  vkDestroyImageView(device, array_view, nullptr);
}

void SceneRenderer::init_csm() {
  const VkImageCreateInfo image_ci{
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_D32_SFLOAT,
      .extent =
          {
              .width = shadow_map_cascade_resolution,
              .height = shadow_map_cascade_resolution,
              .depth = 1u,
          },
      .mipLevels = 1u,
      .arrayLayers = shadow_map_cascade_count,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
               VK_IMAGE_USAGE_SAMPLED_BIT,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
  };
  const VmaAllocationCreateInfo alloc_ci{
      .usage = VMA_MEMORY_USAGE_GPU_ONLY,
  };
  vmaCreateImage(ctx.allocator, &image_ci, &alloc_ci, &csm.image,
                 &csm.allocation, nullptr);

  const VkImageViewCreateInfo array_view_ci{
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = csm.image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
      .format = VK_FORMAT_D32_SFLOAT,
      .subresourceRange =
          {
              .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
              .levelCount = 1u,
              .layerCount = shadow_map_cascade_count,
          },
  };
  vkCreateImageView(ctx.device, &array_view_ci, nullptr, &csm.array_view);

  // Per-layer views for rendering into each cascade
  for (u32 i = 0u; i < shadow_map_cascade_count; ++i) {
    const VkImageViewCreateInfo layer_ci{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = csm.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_D32_SFLOAT,
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                .levelCount = 1u,
                .baseArrayLayer = i,
                .layerCount = 1u,
            },
    };
    vkCreateImageView(ctx.device, &layer_ci, nullptr, &csm.layer_views[i]);
  }

  csm.bindless_handle =
      register_external_view(csm.array_view, VK_IMAGE_VIEW_TYPE_2D_ARRAY);
  info("CSM handle: {}", csm.bindless_handle.index());
  bindless.need_repopulate = true;

  ctx.transition_to_general(csm.image, VK_IMAGE_ASPECT_DEPTH_BIT, 1,
                            shadow_map_cascade_count);
}

} // namespace dy
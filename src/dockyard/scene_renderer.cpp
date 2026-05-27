#include "dockyard/texture_upload_pool.hpp"
#include "glm/ext/matrix_float4x4.hpp"
#include <dockyard/scene_renderer.hpp>

#include <dockyard/device_geometry.hpp>
#include <dockyard/mesh.hpp>
#include <dockyard/shader_watcher.hpp>
#include <dockyard/vfs.hpp>

#include <atomic>
#include <execution>
#include <limits>
#include <ranges>

#include <glm/gtc/packing.hpp>

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
    splits[i] = lambda * log_split + (1.0F - lambda) * uni_split;
  }
  return splits;
}

// Generic — works for both standard and reverse-Z projections.
// Reverse-Z: near_z -> 1.0, far_z -> 0.0.
auto split_to_ndc_z(const glm::mat4 &proj, float view_z) -> float {
  const glm::vec4 clip = proj * glm::vec4(0.0F, 0.0F, view_z, 1.0F);
  return clip.z / clip.w;
}

// z_near_ndc / z_far_ndc are in the camera's NDC convention.
// Reverse-Z: pass z_near_ndc=1.0, z_far_ndc<1.0 for each cascade.
auto frustum_corners_world(const glm::mat4 &inv_view_proj, float z_near_ndc,
                           float z_far_ndc) -> std::array<glm::vec3, 8> {
  const glm::vec4 ndc[8] = {
      {-1.0F, 1.0F, z_near_ndc, 1.0F}, {1.0F, 1.0F, z_near_ndc, 1.0F},
      {1.0F, -1.0F, z_near_ndc, 1.0F}, {-1.0F, -1.0F, z_near_ndc, 1.0F},
      {-1.0F, 1.0F, z_far_ndc, 1.0F},  {1.0F, 1.0F, z_far_ndc, 1.0F},
      {1.0F, -1.0F, z_far_ndc, 1.0F},  {-1.0F, -1.0F, z_far_ndc, 1.0F},
  };

  std::array<glm::vec3, 8> corners;
  for (u32 i = 0; i < 8; ++i) {
    const glm::vec4 world = inv_view_proj * ndc[i];
    const float w = std::abs(world.w) > 1e-5f ? world.w : 1.0F;
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
  glm::vec3 center(0.0F);
  for (const auto &c : corners)
    center += c;
  center /= 8.0F;

  float radius = 0.0F;
  for (const auto &c : corners)
    radius = std::max(radius, glm::distance(c, center));

  // Snap radius to texel grid to prevent cascade size shimmer.
  radius = std::ceil(radius * 16.0F) / 16.0F;

  // --- 2. Light-view matrix ---
  // Eye is placed behind the scene *in the direction of the sun*, so the
  // camera looks from the sun's side toward the scene (correct orientation).
  //
  // z_extent is how far the eye is from center in light-view space.
  // The 500-unit margin buys depth for shadow casters behind the view frustum.
  const glm::vec3 up = std::abs(light_toward_sun.y) > 0.99f
                           ? glm::vec3(0.0F, 0.0F, 1.0F)
                           : glm::vec3(0.0F, 1.0F, 0.0F);
  const float z_extent = radius + 500.0F;
  const glm::vec3 eye = center + light_toward_sun * z_extent;
  glm::mat4 light_view = glm::lookAtLH(eye, center, up);

  // --- 3. Texel snapping ---
  // Transform the world origin into light-view "texel" space, round it, and
  // apply the residual as a translation correction.  This keeps the shadow
  // texel grid stationary as the camera moves, eliminating edge crawl.
  const float texels_per_unit =
      static_cast<float>(shadow_map_cascade_resolution) / (radius * 2.0F);
  glm::vec4 shadow_origin = light_view * glm::vec4(0.0F, 0.0F, 0.0F, 1.0F);
  shadow_origin *= texels_per_unit;

  const glm::vec4 rounded = glm::round(shadow_origin);
  glm::vec4 offset = (rounded - shadow_origin) / texels_per_unit;
  offset.z = 0.0F; // never shift depth — that would break near/far
  offset.w = 0.0F;
  light_view[3] += offset;

  // --- 4. Orthographic projection ---
  // In light-view space the eye is at the origin.  The bounding sphere center
  // is at Z = z_extent (along +Z, since lookAtLH makes +Z point toward center).
  // The sphere occupies [z_extent - radius, z_extent + radius] on the Z axis.
  //
  // We extend near/far by caster_margin to capture shadow casters that lie
  // outside the camera frustum but still cast into it.
  const float caster_margin = 500.0F;
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

auto make_default_override_materials(u32) -> std::vector<GPUMaterial>;
auto grow_pool(SceneRenderer &renderer) -> u32 {
  const u32 old_capacity = renderer.override_pool.capacity;
  const u32 new_capacity = old_capacity * 2;

  info("MaterialOverridePool growing {} → {} slots", old_capacity,
       new_capacity);

  vkDeviceWaitIdle(renderer.ctx.device);

  renderer.geometry_pool->reserve_materials(new_capacity - old_capacity);

  auto new_data = make_default_override_materials(new_capacity);

  if (renderer.override_pool.next > 0) {
    auto live = renderer.geometry_pool->get_materials(
        renderer.override_pool.base_slot, renderer.override_pool.next);
    std::ranges::reverse_copy(live, new_data.begin());
  }

  const auto new_offset =
      renderer.geometry_pool->allocate_materials(std::span(new_data));
  const u32 delta = new_offset.start_index - renderer.override_pool.base_slot;

  for (auto &s : renderer.override_pool.free_slots)
    s += delta;

  renderer.override_pool.base_slot = new_offset.start_index;
  renderer.override_pool.capacity = new_capacity;
  renderer.override_pool.needs_grow = false;
  renderer.bindless.mark_dirty();

  return delta;
}

auto make_default_override_materials(u32 count) -> std::vector<GPUMaterial> {
  std::vector<GPUMaterial> output(count);
  for (auto &material : output) {
    material.albedo_factor[0] = material.albedo_factor[1] =
        material.albedo_factor[2] = material.albedo_factor[3] = 1.F;
    material.roughness_factor = 1.F;
    material.normal_scale = 1.F;
    material.occlusion_strength = 1.F;
    material.albedo_index = 0U; // white fallback
  }
  return output;
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

auto SceneRenderer::register_gltf(MeshAsset &&asset) -> MeshAssetHandle {
  return mesh_registry.create(std::move(asset));
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
  return handle;
}

auto SceneRenderer::get_mesh(MeshAssetHandle handle) -> MeshAsset * {
  return mesh_registry.get(handle);
}
auto SceneRenderer::get_mesh(MeshAssetHandle handle) const
    -> const MeshAsset * {
  return mesh_registry.get(handle);
}

auto SceneRenderer::get_material_view(MeshAssetHandle handle) const
    -> ConstMaterialView {
  const auto *mesh = get_mesh(handle);
  assert(mesh && "invalid mesh handle");
  return get_material_view(*mesh);
}

auto SceneRenderer::get_material_view_mut(MeshAssetHandle handle)
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

auto SceneRenderer::remove_override(Entity entity) -> void {
  auto *material_override = entity.try_get<Components::MaterialOverride>();
  if (material_override == nullptr)
    return;

  if (material_override->gpu_slot != ~0U)
    override_pool.free(material_override->gpu_slot);

  entity.remove<Components::MaterialOverride>();
}

constexpr std::byte operator""_b(unsigned long long val) {
  return static_cast<std::byte>(val);
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
  // 1. Define your 1x1 pixel data as byte arrays
  constexpr auto white_pixel = std::array{255_b, 255_b, 255_b, 255_b};
  constexpr auto blue_pixel =
      std::array{127_b, 127_b, 255_b, 255_b}; // 0.5, 0.5, 1.0, 1.0
  constexpr auto mr_pixel =
      std::array{0_b, 255_b, 0_b, 255_b}; // 0.0, 1.0, 0.0, 1.0
  constexpr auto occlusion_pixel = std::array{255_b, 255_b, 255_b, 255_b};
  constexpr auto black_pixel = std::array{0_b, 0_b, 0_b, 255_b};
  white_texture = upload_texture(white_pixel, "white_fallback_texture", 1, 1,
                                 VK_FORMAT_R8G8B8A8_UNORM, false);
  normal_texture = upload_texture(blue_pixel, "normal_fallback_texture", 1, 1,
                                  VK_FORMAT_R8G8B8A8_UNORM, false);
  metallic_roughness_texture =
      upload_texture(mr_pixel, "metallic_roughness_fallback_texture", 1, 1,
                     VK_FORMAT_R8G8B8A8_UNORM, false);
  occlusion_texture =
      upload_texture(occlusion_pixel, "occlusion_fallback_texture", 1, 1,
                     VK_FORMAT_R8G8B8A8_UNORM, false);
  black_texture = upload_texture(black_pixel, "black_fallback_texture", 1, 1,
                                 VK_FORMAT_R8G8B8A8_UNORM, false);

  assert(white_texture.index() == 0);
  info("White texture index: {}", white_texture.index());
  dummy_texture_handle = white_texture;

  texture_upload_pool = std::make_unique<pool::TextureUploadPool>();

  resize();
}
auto SceneRenderer::initialise_bindless() -> void {
  constexpr u32 override_material_count_initial = 16U;
  auto blank = make_default_override_materials(override_material_count_initial);
  auto offset = geometry_pool->allocate_materials(std::span(blank));
  override_pool = {
      .base_slot = offset.start_index,
      .capacity = override_material_count_initial,
  };

  const VkSamplerCreateInfo sampler_ci{
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_LINEAR,
      .minFilter = VK_FILTER_LINEAR,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
      .mipLodBias = 0.0F,
      .anisotropyEnable = VK_TRUE,
      .maxAnisotropy = 16,
      .compareEnable = VK_FALSE,
      .minLod = 0.0F,
      .maxLod = 1.0F,
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
      .minLod = 0.0F,
      .maxLod = 1.0F,
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
                /*initial_accel_structs       =*/0u,
                /*initial_sub_images       =*/512u);

  pipeline_layout = create_main_pipeline_layout(ctx.device, bindless.layout);
  pipeline_registry = std::make_unique<PipelineRegistry>(ctx.device);

  {
    auto result = pipeline_registry->create_graphics({
        .shader_path = VFSPath::create("shaders://composite.slang"),
        .descriptor_set_layout = bindless.layout,
        .render_targets = {.color_formats = {VK_FORMAT_R8G8B8A8_SRGB}},
        .cull_mode = VK_CULL_MODE_NONE,
        .blending = {BlendMode::opaque()},
    });
    if (!result) {
      error("composite pipeline initialization failed: {}", result.error());
      std::abort();
    }
    composite_pipeline = *result;
  }

  {
    auto result = pipeline_registry->create_graphics({
        .shader_path = VFSPath::create("shaders://skybox.slang"),
        .descriptor_set_layout = bindless.layout,
        .render_targets =
            {
                .color_formats = {VK_FORMAT_R16G16B16A16_SFLOAT},
                .depth_format = VK_FORMAT_D32_SFLOAT,
            },
        .cull_mode = VK_CULL_MODE_NONE,
        .samples = VK_SAMPLE_COUNT_4_BIT,
        .blending = {BlendMode::opaque()},
    });

    if (!result) {
      error("skybox pipeline initialization failed: {}", result.error());
      std::abort();
    }
    skybox_pipeline = *result;
  }

  {
    auto result = pipeline_registry->create_compute({
        .shader_path = VFSPath::create("shaders://culling.slang"),
        .descriptor_set_layout = bindless.layout,
        .layout = VK_NULL_HANDLE,
    });
    if (!result) {
      error("culling pipeline initialization failed: {}", result.error());
      std::abort();
    }
    culling_pipeline = *result;
  }

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
      .minLod = 0.0F,
      .maxLod = 1.0F,
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

  {
    auto equirect_tex = Texture::load_hdr_texture(
        ctx, VFSPath::create("textures://env/sunset.hdr"));

    auto equirect = textures.create(TextureEntry{
        .texture = std::move(equirect_tex),
        .sampled_view_type = VK_IMAGE_VIEW_TYPE_2D,
    });

    ibl_probe = IblProbe::create(ctx, *this, equirect);
  }
}
auto SceneRenderer::upload_texture(std::span<const std::byte> data,
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

  bindless.mark_dirty();
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

void SceneRenderer::submit(MeshAssetHandle handle, const glm::mat4 &t,
                           u32 pipeline_id, u32 material_id) {
  auto *asset = get_mesh(handle);
  if (asset == nullptr)
    return;

  for (const auto &node : asset->nodes) {
    const glm::mat4 node_transform = t * node.local_transform;

    for (const auto &prim : node.primitives) {
      const u32 id = static_cast<u32>(submission_queue.size());
      const u32 resolved_mat =
          material_id != 0 ? material_id : prim.material_id;
      const auto &lg = prim.lod_group;

      submission_queue.emplace_back(PendingDraw{
          .lod_group = &lg,
          .pipeline_id = pipeline_id,
          .material_id = resolved_mat,
          .transform = node_transform,
          .aabb = prim.aabb,
          .instance_id = id,
      });

      // Depth — one bucket per primitive; lod_group carries all levels
      {
        const u64 depth_key = static_cast<u64>(lg.lods[0].first_index);
        auto &b = depth_prepass.buckets[depth_key];
        b.lod_group = &lg;
        b.pipeline_id = pipeline_id;
        b.instance_ids.push_back(id);
      }

      // Forward — one bucket per primitive
      {
        const u64 fwd_key =
            (static_cast<u64>(pipeline_id) << 48) |
            (static_cast<u64>(resolved_mat) << 32) |
            static_cast<u64>(lg.lods[0].first_index & 0xFFFFFFFFu);
        auto &b = forward_pass.buckets[fwd_key];
        b.lod_group = &lg;
        b.pipeline_id = pipeline_id;
        b.material_id = resolved_mat;
        b.instance_ids.push_back(id);
      }
    }
  }
}

void SceneRenderer::ensure_global_capacity(usize instance_count) {
  if (const auto size = instance_count * sizeof(CompressedInstanceData);
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
                            const glm::mat4 &projection) -> PrepareResult {
  {
    texture_upload_pool->poll_one(*this); // at most one, never stalls
  }

  {
    pipeline_registry->poll_and_update_dirty_pipelines();
  }

  if (override_pool.needs_grow) [[unlikely]] {
    const u32 delta = grow_pool(*this);
    return {
        .status = PrepareResult::Status::SuccessMaterialPoolGrew,
        .material_pool_delta = delta,
    };
  }

  current_frame_index = frame_index;
  if (submission_queue.empty()) {
    return {
        .status = PrepareResult::Status::SuccessNoSubmissions,
    };
  }

  ensure_global_capacity(submission_queue.size());
  global_instance_data.clear();

  for (const auto &draw : submission_queue) {
    const auto half = (draw.aabb.get_max() - draw.aabb.get_min()) * 0.5F;
    const auto radius = glm::length(half);
    global_instance_data.emplace_back(draw.transform, draw.material_id, radius,
                                      draw.lod_group->lod_count);
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
      .cascades = csm_frame_data.cascades,
      .frustum_planes = extract_frustum_planes(view_proj),
      .camera_position = inv_view[3],
      .sun_direction = sun_direction,
      .shadow_array_index = csm_frame_data.shadow_array_index,
      .shadow_sampler_index = csm_frame_data.shadow_sampler_index,
      .ibl_irradiance_index = ibl_probe.irradiance.index(),
      .ibl_prefiltered_index = ibl_probe.prefiltered.index(),
      .ibl_brdf_lut_index = ibl_probe.brdf_lut.index(),
      .ibl_sampler_index = dummy_sampler_handle.index(),
      .ibl_prefiltered_mips = ibl_probe.prefiltered_mip_count,
  };

  frame_ubo_buffers.at(frame_index)->upload(std::span(&ubo, 1));
  depth_prepass.bake(global_instance_data.size());
  forward_pass.bake(global_instance_data.size());

  for (auto &v : depth_prepass.buckets | std::views::values) {
    v.instance_ids.clear();
  }
  for (auto &v : forward_pass.buckets | std::views::values) {
    v.instance_ids.clear();
  }
  submission_queue.clear();
  return {
      .status = PrepareResult::Status::Success,
  };
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
    const VkPipeline pipe =
        override_pipeline != VK_NULL_HANDLE
            ? override_pipeline
            : pipeline_registry->get_unsafe(batch.pipeline_id);
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

void SceneRenderer::skybox_pass(VkCommandBuffer cmd) {
  const auto &entry = pipeline_registry->get_entry(skybox_pipeline);

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, entry.pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, entry.layout,
                          0u, 1u, &bindless.set, 0u, nullptr);

  struct SkyboxPushConstants {
    u32 env_map_index;
    u32 sampler_index;
    glm::mat4 inv_view_proj;
  };

  const auto frame_data =
      frame_ubo_buffers.at(current_frame_index)->read<FrameUBO>();
  const auto inv_view_projection_no_translation = glm::inverse(
      frame_data.projection * glm::mat4(glm::mat3(frame_data.view)));

  const SkyboxPushConstants push{
      .env_map_index = ibl_probe.env_map.index(),
      .sampler_index = dummy_sampler_handle.index(),
      .inv_view_proj = inv_view_projection_no_translation,
  };

  vkCmdPushConstants(cmd, entry.layout,
                     VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                     0u, sizeof(push), &push);
  vkCmdDraw(cmd, 3u, 1u, 0u, 0u);
}

void SceneRenderer::composite_pass(VkCommandBuffer cmd) {
  const auto &entry = pipeline_registry->get_entry(composite_pipeline);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, entry.pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, entry.layout,
                          0u, 1u, &bindless.set, 0u, nullptr);
  const CompositePushConstants push{
      .forward_texture_index = forward_target_handle,
      .sampler = dummy_sampler_handle,
  };
  vkCmdPushConstants(cmd, entry.layout,
                     VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                     0u, sizeof(push), &push);
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

  const auto &entry = pipeline_registry->get_entry(culling_pipeline);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, entry.pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, entry.layout, 0U,
                          1U, &bindless.set, 0U, nullptr);
  vkCmdPushConstants(cmd, entry.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0U,
                     sizeof(push), &push);

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

auto RenderPass::bake(usize total_global_instances) -> void {
  std::vector<PaddedDrawCommand> commands;
  std::vector<u32> remapped_indices;
  std::vector<u32> draw_counts;
  std::vector<u32> instance_to_commands(total_global_instances, 0xFFFFFFFFu);

  batches.clear();
  u32 current_pipeline = ~0U;

  for (auto &[key, bucket] : buckets) {
    if (bucket.pipeline_id != current_pipeline) {
      current_pipeline = bucket.pipeline_id;
      draw_counts.push_back(0u);
      batches.push_back({
          .pipeline_id = bucket.pipeline_id,
          .max_command_count = 0U,
          .first_command_index = static_cast<u32>(commands.size()),
          .count_buffer_offset =
              static_cast<u32>((draw_counts.size() - 1) * sizeof(u32)),
      });
    }

    const u32 lod0_cmd_idx = static_cast<u32>(commands.size());
    for (u32 iid : bucket.instance_ids) {
      instance_to_commands[iid] = lod0_cmd_idx;
    }

    const auto &lod_group = bucket.lod_group;
    for (u8 lod = 0; lod < lod_group->lod_count; ++lod) {
      const u32 unique_first_instance =
          static_cast<u32>(remapped_indices.size());

      for (u32 iid : bucket.instance_ids) {
        remapped_indices.push_back(iid);
      }

      const auto m = lod_group->resolve(lod);
      commands.push_back({
          .index_count = m.index_count,
          .instance_count = 0U,
          .first_index = m.first_index,
          .vertex_offset = m.vertex_offset,
          .first_instance = unique_first_instance,
      });

      batches.back().max_command_count++;
      draw_counts.back()++;
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

  float prev_ndc = 1.0F;

  for (u32 i = 0u; i < shadow_map_cascade_count; ++i) {
    const float view_z = splits[i];
    const float curr_ndc = split_to_ndc_z(proj, view_z);

    csm_frame_data.cascades[i] =
        compute_cascade(view, proj, prev_ndc, curr_ndc, view_z, sun_direction);

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

  ctx.transition_to_general(csm.image, VK_IMAGE_ASPECT_DEPTH_BIT, 1,
                            shadow_map_cascade_count);
}

CompressedInstanceData::CompressedInstanceData(const glm::mat4 &t,
                                               u16 material_id,
                                               f32 bounding_radius,
                                               u8 lod_count) {
  transform = glm::mat3x4(glm::transpose(t));

  const u32 meta = (static_cast<u32>(material_id) & 0xFFFFu) |
                   ((static_cast<u32>(lod_count) & 0x7u) << 16u);
  material_and_lod = std::bit_cast<float>(meta);

  // fp16 radius compression isn't worth it — you have the space
  // and half-float precision breaks for large world coordinates
  this->bounding_radius = bounding_radius;
  padding0 = 0.0f;
  padding1 = 0.0f;
}

} // namespace dy
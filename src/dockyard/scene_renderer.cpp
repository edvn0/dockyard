#include "dockyard/texture.hpp"
#include <algorithm>
#include <dockyard/crash_reporter.hpp>
#include <dockyard/scene_renderer.hpp>

#include <imgui.h>

#include <dockyard/device_geometry.hpp>
#include <dockyard/mesh.hpp>
#include <dockyard/shader_watcher.hpp>
#include <dockyard/texture_upload_pool.hpp>
#include <dockyard/vfs.hpp>

#include <atomic>
#include <execution>
#include <limits>
#include <numeric>
#include <ranges>

#include <glm/ext/matrix_float4x4.hpp>
#include <glm/gtc/packing.hpp>

#include <tracy/Tracy.hpp>
#include <tracy/TracyVulkan.hpp>

namespace dy {

struct ProfilingContext {
  TracyVkCtx ctx{};
};

namespace {

auto compute_cascade_splits(float near_z, float far_z, float lambda = 0.85f)
    -> std::array<float, shadow_map_cascade_count> {
  ZoneScopedNC("compute_cascade_splits", 0xAAAAAA);
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

auto split_to_ndc_z(const glm::mat4 &proj, float view_z) -> float {
  const glm::vec4 clip = proj * glm::vec4(0.0F, 0.0F, view_z, 1.0F);
  return clip.z / clip.w;
}

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

auto compute_cascade(const glm::mat4 &camera_view, const glm::mat4 &camera_proj,
                     float prev_split_ndc, float curr_split_ndc,
                     float curr_split_view, const glm::vec3 &light_toward_sun)
    -> CascadeData {
  ZoneScopedNC("compute_cascade", 0xFFAA00);
  const glm::mat4 inv_view_proj = glm::inverse(camera_proj * camera_view);
  const auto corners =
      frustum_corners_world(inv_view_proj, prev_split_ndc, curr_split_ndc);

  glm::vec3 center(0.0F);
  for (const auto &c : corners)
    center += c;
  center /= 8.0F;

  float radius = 0.0F;
  for (const auto &c : corners)
    radius = std::max(radius, glm::distance(c, center));

  radius = std::ceil(radius * 16.0F) / 16.0F;

  const glm::vec3 up = std::abs(light_toward_sun.y) > 0.99f
                           ? glm::vec3(0.0F, 0.0F, 1.0F)
                           : glm::vec3(0.0F, 1.0F, 0.0F);
  const float z_extent = radius + 500.0F;
  const glm::vec3 eye = center + light_toward_sun * z_extent;
  glm::mat4 light_view = glm::lookAtLH(eye, center, up);

  const float texels_per_unit =
      static_cast<float>(shadow_map_cascade_resolution) / (radius * 2.0F);
  glm::vec4 shadow_origin = light_view * glm::vec4(0.0F, 0.0F, 0.0F, 1.0F);
  shadow_origin *= texels_per_unit;

  const glm::vec4 rounded = glm::round(shadow_origin);
  glm::vec4 offset = (rounded - shadow_origin) / texels_per_unit;
  offset.z = 0.0F;
  offset.w = 0.0F;
  light_view[3] += offset;

  const float caster_margin = 500.0F;
  const float ortho_near = z_extent - radius - caster_margin;
  const float ortho_far = z_extent + radius + caster_margin;

  const glm::mat4 light_proj =
      glm::orthoLH_ZO(-radius, radius, -radius, radius, ortho_near, ortho_far);

  return {
      .view_proj = light_proj * light_view,
      .split_depth = curr_split_view,
  };
}

auto make_default_override_materials(u32) -> std::vector<GPUMaterial>;
auto grow_pool(SceneRenderer &renderer) -> u32 {
  ZoneScopedNC("grow_pool", 0xFF4500);
  const u32 old_capacity = renderer.override_pool.capacity;
  const u32 new_capacity = old_capacity * 2;

  info("[Renderer] grow_pool(): {} -> {} slots, base_slot={} next={} free={}",
       old_capacity, new_capacity, renderer.override_pool.base_slot,
       renderer.override_pool.next, renderer.override_pool.free_slots.size());

  vkDeviceWaitIdle(renderer.ctx.device);

  renderer.geometry_pool->reserve_materials(new_capacity - old_capacity);

  auto new_data = make_default_override_materials(new_capacity);

  if (renderer.override_pool.next > 0) {
    auto live = renderer.geometry_pool->get_materials(
        renderer.override_pool.base_slot, renderer.override_pool.next);
    std::ranges::copy(live, new_data.begin());
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

  info("[Renderer] grow_pool(): done, new base_slot={} delta={}",
       renderer.override_pool.base_slot, delta);
  return delta;
}

auto make_default_override_materials(u32 count) -> std::vector<GPUMaterial> {
  ZoneScopedNC("make_default_override_materials", 0x888888);
  std::vector<GPUMaterial> output(count);
  for (auto &material : output) {
    material.albedo_factor[0] = material.albedo_factor[1] =
        material.albedo_factor[2] = material.albedo_factor[3] = 1.F;
    material.roughness_factor = 1.F;
    material.normal_scale = 1.F;
    material.occlusion_strength = 1.F;
    material.albedo_index = 0U;
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
  ZoneScopedNC("SceneRenderer::register_gltf", 0x00BFFF);
  const usize table_before = flat_prim_table.size();
  for (auto &node : asset.nodes) {
    for (auto &prim : node.primitives) {
      prim.flat_index = static_cast<u32>(flat_prim_table.size());
      flat_prim_table.push_back({
          .first_index = prim.lod_group.lods[0].first_index,
          .index_count = prim.lod_group.lods[0].index_count,
          .vertex_offset = prim.lod_group.vertex_offset,
          .default_material_id = prim.material_id,
          .lod_group = nullptr,
      });
    }
  }

  auto handle = mesh_registry.create(std::move(asset));

  auto *stored = mesh_registry.get(handle);
  for (auto &node : stored->nodes) {
    for (auto &prim : node.primitives) {
      flat_prim_table[prim.flat_index].lod_group = &prim.lod_group;
    }
  }

  const auto name = asset.source_path.view();
  info("[Renderer] register_gltf({}): handle={} flat_prim_table {} -> {} "
       "entries",
       name.empty() ? "Missing" : name, handle.index(), table_before,
       flat_prim_table.size());
  return handle;
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
  ZoneScopedNC("SceneRenderer::remove_override", 0xFF6347);
  auto *material_override = entity.try_get<Components::MaterialOverride>();
  if (material_override == nullptr)
    return;

  if (material_override->gpu_slot !=
      Components::MaterialOverride::invalid_material)
    override_pool.free(material_override->gpu_slot);

  entity.remove<Components::MaterialOverride>();
}

constexpr std::byte operator""_b(unsigned long long val) {
  return static_cast<std::byte>(val);
}

SceneRenderer::SceneRenderer(VulkanContext &c, SwapchainResources &sc)
    : ctx(c), swapchain(sc),
      tracy_vk_ctx(
          std::unique_ptr<ProfilingContext,
                          decltype(+[](ProfilingContext *) -> void {})>{
              new ProfilingContext{},
              +[](ProfilingContext *ctx) { delete ctx; }}),
      depth_prepass(RenderPassType::DepthPrepass, ctx.allocator, *this),
      forward_pass(RenderPassType::Forward, ctx.allocator, *this) {
  ZoneScopedNC("SceneRenderer::SceneRenderer", 0x4169E1);

  constexpr auto vertex_count = 1'000'000;
  constexpr auto index_count = 10'000'000;
  constexpr auto material_count = 500;
  geometry_pool = GeometryPool::create(
      ctx.allocator, vertex_count * sizeof(Vertex),
      vertex_count * sizeof(PositionOnlyVertex), index_count * sizeof(u32),
      material_count * sizeof(GPUMaterial));

  {
    ZoneScopedNC("Override Pool Init", 0x888888);
    constexpr u32 override_material_count_initial = 16U;
    auto blank =
        make_default_override_materials(override_material_count_initial);
    auto offset = geometry_pool->allocate_materials(std::span(blank));
    override_pool = {
        .base_slot = offset.start_index,
        .capacity = override_material_count_initial,
    };
  }

  constexpr auto white_pixel = std::array{255_b, 255_b, 255_b, 255_b};
  constexpr auto blue_pixel = std::array{127_b, 127_b, 255_b, 255_b};
  constexpr auto mr_pixel = std::array{0_b, 255_b, 0_b, 255_b};
  constexpr auto occlusion_pixel = std::array{255_b, 255_b, 255_b, 255_b};
  constexpr auto black_pixel = std::array{0_b, 0_b, 0_b, 255_b};
  white_texture = upload_texture(white_pixel, "white_fallback_texture", 1, 1,
                                 VK_FORMAT_R8G8B8A8_UNORM, false);
  normal_texture = upload_texture(blue_pixel, "normal_fallback_texture", 1, 1,
                                  VK_FORMAT_R8G8B8A8_UNORM, false);
  metallic_roughness_texture =
      upload_texture(mr_pixel, "metallic_roughness_fallback_texture", 1, 1,
                     VK_FORMAT_R8G8B8A8_UNORM, false);
  ambient_occlusion_texture =
      upload_texture(occlusion_pixel, "occlusion_fallback_texture", 1, 1,
                     VK_FORMAT_R8G8B8A8_UNORM, false);
  black_texture = upload_texture(black_pixel, "black_fallback_texture", 1, 1,
                                 VK_FORMAT_R8G8B8A8_UNORM, false);

  assert(white_texture.index() == 0);
  info("White texture index: {}", white_texture.index());
  dummy_texture_handle = white_texture;

  texture_upload_pool = std::make_unique<pool::TextureUploadPool>();

  resize();

  tracy_vk_ctx->ctx = TracyVkContextHostCalibrated(
      ctx.instance.instance, ctx.physical_device, ctx.device,
      vkGetInstanceProcAddr, vkGetDeviceProcAddr);
  TracyVkContextName(tracy_vk_ctx->ctx, "main_gfx", 8);
}

auto SceneRenderer::initialise_settings() -> void {
  settings_registry.add(
      "Skybox", [this] { ImGui::SliderFloat("LOD", &skybox_lod, 0.0F, 8.0F); });
  settings_registry.add("Bloom", [this] {
    ImGui::Checkbox("Enabled", &bloom_enabled);
    ImGui::BeginDisabled(!bloom_enabled);
    ImGui::SliderFloat("Threshold", &bloom_threshold, 0.0F, 4.0F);
    ImGui::SliderFloat("Strength", &bloom_strength, 0.0F, 0.5F);
    ImGui::SliderFloat("Scatter", &bloom_scatter, 0.0F, 1.0F);
    ImGui::EndDisabled();
  });
}

auto SceneRenderer::initialise_bindless() -> void {
  ZoneScopedNC("SceneRenderer::initialise_bindless", 0x4169E1);

  {
    ZoneScopedNC("Sampler Creation", 0x708090);
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
        .maxLod = VK_LOD_CLAMP_NONE,
        .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
        .unnormalizedCoordinates = VK_FALSE,
    };
    vk::check(
        vkCreateSampler(ctx.device, &sampler_ci, nullptr, &dummy_sampler_vk));
    dummy_sampler_handle =
        samplers.create(SamplerEntry{.sampler = dummy_sampler_vk});

    const VkSamplerCreateInfo hiz_sampler_ci{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .mipLodBias = 0.0f,
        .anisotropyEnable = VK_FALSE,
        .maxAnisotropy = 1.0f,
        .compareEnable = VK_FALSE,
        .minLod = 0.0f,
        .maxLod = VK_LOD_CLAMP_NONE,
        .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,
        .unnormalizedCoordinates = VK_FALSE,
    };

    VkSampler hiz_sampler_vk{};
    vk::check(
        vkCreateSampler(ctx.device, &hiz_sampler_ci, nullptr, &hiz_sampler_vk));
    hiz_sampler_handle =
        samplers.create(SamplerEntry{.sampler = hiz_sampler_vk});

    constexpr VkSamplerCreateInfo comparison_ci{
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

    const VkSamplerCreateInfo shadow_ci{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
        .compareEnable = VK_TRUE,
        .compareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
        .minLod = 0.0F,
        .maxLod = 1.0F,
        .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,
    };
    vk::check(vkCreateSampler(ctx.device, &shadow_ci, nullptr,
                              &shadow_comparison_sampler_vk));
    shadow_sampler_bindless_idx =
        comparison_samplers
            .create(SamplerEntry{.sampler = shadow_comparison_sampler_vk})
            .index();

    const VkSamplerCreateInfo cube_sampler_ci{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .mipLodBias = 0.0f,
        .anisotropyEnable = VK_FALSE,
        .compareEnable = VK_FALSE,
        .minLod = 0.0f,
        .maxLod = VK_LOD_CLAMP_NONE,
        .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
        .unnormalizedCoordinates = VK_FALSE,
    };

    VkSampler cube_sampler_vk{};
    vk::check(vkCreateSampler(ctx.device, &cube_sampler_ci, nullptr,
                              &cube_sampler_vk));
    cube_sampler_handle =
        samplers.create(SamplerEntry{.sampler = cube_sampler_vk});
  }

  {
    ZoneScopedNC("Bindless Set Init", 0x20B2AA);
    const BindlessCaps caps = query_bindless_caps(ctx.physical_device);
    bindless.init(ctx.device, caps,
                  /*initial_textures            =*/1024u,
                  /*initial_samplers            =*/64u,
                  /*initial_comparison_samplers =*/8u,
                  /*initial_storage_images      =*/1024u,
                  /*initial_accel_structs       =*/0u,
                  /*initial_sub_images       =*/512u);

    pipeline_layout = create_main_pipeline_layout(ctx.device, bindless.layout);
  }

  {
    ZoneScopedNC("Pipeline Registry Init", 0x9370DB);
    pipeline_registry = std::make_unique<PipelineRegistry>(ctx);

    {
      ZoneScopedNC("Composite Pipeline", 0x9370DB);
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
      ZoneScopedNC("Bloom Downsample Pipeline", 0xFF69B4);
      auto result = pipeline_registry->create_compute({
          .shader_path = VFSPath::create("shaders://bloom_downsample.slang"),
          .descriptor_set_layout = bindless.layout,
          .layout = VK_NULL_HANDLE,
      });
      if (!result) {
        error("bloom_downsample pipeline initialization failed: {}",
              result.error());
        std::abort();
      }
      bloom_downsample_pipeline = *result;
    }

    {
      ZoneScopedNC("Bloom Blur Pipeline", 0xFF69B4);
      auto result = pipeline_registry->create_compute({
          .shader_path = VFSPath::create("shaders://bloom_blur.slang"),
          .descriptor_set_layout = bindless.layout,
          .layout = VK_NULL_HANDLE,
      });
      if (!result) {
        error("bloom_blur pipeline initialization failed: {}", result.error());
        std::abort();
      }
      bloom_blur_pipeline = *result;
    }

    {
      ZoneScopedNC("Skybox Pipeline", 0x9370DB);
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
      ZoneScopedNC("Buffer reset pipeline", 0x9370DB);
      auto result = pipeline_registry->create_compute({
          .shader_path = VFSPath::create("shaders://buffer_reset.slang"),
          .descriptor_set_layout = bindless.layout,
          .layout = VK_NULL_HANDLE,
      });
      if (!result) {
        error("buffer reset initialization failed: {}", result.error());
        std::abort();
      }
      buffer_reset_pipeline = *result;
    }

    {
      ZoneScopedNC("Light Clustering Pipeline", 0xFFD700);
      auto result = pipeline_registry->create_compute({
          .shader_path = VFSPath::create("shaders://light_clustering.slang"),
          .descriptor_set_layout = bindless.layout,
          .layout = VK_NULL_HANDLE,
      });
      if (!result) {
        error("light clustering pipeline initialization failed: {}",
              result.error());
        std::abort();
      }
      light_clustering_pipeline = *result;
    }

    {
      ZoneScopedNC("Depth-Only Culling Pipeline", 0x9370DB);
      auto result = pipeline_registry->create_compute({
          .shader_path = VFSPath::create("shaders://depth_only_culling.slang"),
          .descriptor_set_layout = bindless.layout,
          .layout = VK_NULL_HANDLE,
      });
      if (!result) {
        error("depth-only culling pipeline initialization failed: {}",
              result.error());
        std::abort();
      }
      depth_only_culling_pipeline = *result;
    }

    {
      ZoneScopedNC("Depth-to-R32 Pipeline", 0x9370DB);
      auto result = pipeline_registry->create_compute({
          .shader_path = VFSPath::create("shaders://depth_to_r32.slang"),
          .descriptor_set_layout = bindless.layout,
          .layout = VK_NULL_HANDLE,
      });
      if (!result) {
        error("depth-to-r32 pipeline initialization failed: {}",
              result.error());
        std::abort();
      }
      depth_to_r32_pipeline = *result;
    }

    {
      ZoneScopedNC("Forward Occlusion Culling Pipeline", 0x9370DB);
      auto result = pipeline_registry->create_compute({
          .shader_path =
              VFSPath::create("shaders://forward_occlusion_culling.slang"),
          .descriptor_set_layout = bindless.layout,
          .layout = VK_NULL_HANDLE,
      });
      if (!result) {
        error("forward occlusion culling pipeline initialization failed: {}",
              result.error());
        std::abort();
      }
      forward_occlusion_pipeline = *result;
    }

    {
      ZoneScopedNC("HiZ Downsample Pipeline", 0x9370DB);
      auto result = pipeline_registry->create_compute({
          .shader_path = VFSPath::create("shaders://hiz_downsample.slang"),
          .descriptor_set_layout = bindless.layout,
          .layout = VK_NULL_HANDLE,
      });
      if (!result) {
        error("hierarchical depth pyramid downsample pipeline initialization "
              "failed: {}",
              result.error());
        std::abort();
      }
      hiz_downsample_pipeline = *result;
    }

    {
      ZoneScopedNC("Skinning Pipeline", 0xFF8C00);
      auto result = pipeline_registry->create_compute({
          .shader_path = VFSPath::create("shaders://skinning.slang"),
          .descriptor_set_layout = bindless.layout,
          .layout = VK_NULL_HANDLE,
      });
      if (!result) {
        error("skinning pipeline initialization failed: {}", result.error());
        std::abort();
      }
      skinning_pipeline = *result;
    }

    {
      ZoneScopedNC("Shadow Pipeline", 0x9370DB);
      auto result = pipeline_registry->create_graphics({
          .shader_path = VFSPath::create("shaders://shadow.slang"),
          .layout = pipeline_layout,
          .render_targets = {.depth_format = VK_FORMAT_D32_SFLOAT},
          .cull_mode = VK_CULL_MODE_BACK_BIT,
          .samples = VK_SAMPLE_COUNT_1_BIT,
          .depth = {.test = true,
                    .write = true,
                    .compare_op = VK_COMPARE_OP_LESS_OR_EQUAL},
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

  init_csm();

  {
    ZoneScopedNC("IBL Probe Init", 0xFFD700);
    pending_hdr_map =
        VFSPath::create("textures://env/kloppenheim_06_puresky_4k.ktx2");
  }
}

auto SceneRenderer::upload_texture(std::span<const std::byte> data,
                                   std::string_view name, u32 w, u32 h,
                                   VkFormat fmt, bool gen_mips, bool storage)
    -> TextureHandle {
  breadcrumb("upload_texture");
  ZoneScopedNC("SceneRenderer::upload_texture", 0x00CED1);
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
  breadcrumb("renderer_resize");
  ZoneScopedNC("SceneRenderer::resize", 0xFFA500);
  for (u32 i = 0U; i < frames_in_flight; ++i) {
    if (frame_ubo_buffers[i]) {
      DeletionQueue::the().push(
          [b = frame_ubo_buffers[i]->get_buffer(),
           a = frame_ubo_buffers[i]->get_allocation(),
           alloc = ctx.allocator] { vmaDestroyBuffer(alloc, b, a); });
      frame_ubo_buffers[i]->detach();
    }
    frame_ubo_buffers[i] =
        Buffer::create(ctx.allocator, "frame_ubo_buffer", sizeof(FrameUBO),
                       VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    if (cluster_list_buffers[i]) {
      DeletionQueue::the().push(
          [b = cluster_list_buffers[i]->get_buffer(),
           a = cluster_list_buffers[i]->get_allocation(),
           alloc = ctx.allocator] { vmaDestroyBuffer(alloc, b, a); });
      cluster_list_buffers[i]->detach();
    }
    cluster_list_buffers[i] = Buffer::create(
        ctx.allocator, "cluster_list", cluster_total * sizeof(ClusterEntry),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    cluster_list_buffers[i]->set_zero();
    if (light_list_buffers[i]) {
      DeletionQueue::the().push(
          [b = light_list_buffers[i]->get_buffer(),
           a = light_list_buffers[i]->get_allocation(),
           alloc = ctx.allocator] { vmaDestroyBuffer(alloc, b, a); });
      light_list_buffers[i]->detach();
    }
    light_list_buffers[i] =
        Buffer::create(ctx.allocator, "light_list",
                       cluster_max_light_list_entries * sizeof(u32),
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    if (light_list_counter_buffers[i]) {
      DeletionQueue::the().push(
          [b = light_list_counter_buffers[i]->get_buffer(),
           a = light_list_counter_buffers[i]->get_allocation(),
           alloc = ctx.allocator] { vmaDestroyBuffer(alloc, b, a); });
      light_list_counter_buffers[i]->detach();
    }
    light_list_counter_buffers[i] =
        Buffer::create(ctx.allocator, "light_list_counter", sizeof(u32),
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  }
}

auto SceneRenderer::destroy() -> void {
  {
    ZoneScopedNC("SceneRenderer::destroy", 0xFF0000);

    csm.destroy(ctx.device, ctx.allocator);
    ibl_probe.destroy(ctx, *this);

    bindless.destroy();

    std::ranges::for_each(textures.mutable_data(),
                          [&c = ctx, this](TexturePool::Slot &v) {
                            if ((v.gen & 1u) == 0u)
                              return;
                            auto &tex = v.object.texture;
                            if (!tex.valid())
                              return;
                            tex.destroy(c, &textures);
                            tex.destroy(c, &subimages);
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
  TracyVkDestroy(tracy_vk_ctx->ctx);
}

void SceneRenderer::submit(MeshAssetHandle handle, const glm::mat4 &t,
                           u32 pipeline_id, u32 material_id,
                           u32 blend_pipeline_id) {
  auto *asset = get_mesh(handle);
  if (asset == nullptr) [[unlikely]] {
    error("[Renderer] submit(): null mesh asset for handle={}", handle.index());
    return;
  }

  for (const auto &node : asset->nodes) {
    const glm::mat4 node_t = t * node.local_transform;
    for (const auto &prim : node.primitives) {
      if (prim.flat_index >= static_cast<u32>(flat_prim_table.size()))
          [[unlikely]] {
        error("[Renderer] submit(): flat_index {} out of bounds (table={}), "
              "handle={}",
              prim.flat_index, flat_prim_table.size(), handle.index());
        continue;
      }
      const u32 resolved_mat =
          material_id != ~0u ? material_id : prim.material_id;

      const auto &gpu_mat = geometry_pool->get_material(resolved_mat);
      const bool is_blend =
          blend_pipeline_id != ~0u &&
          gpu_mat.alpha_mode == static_cast<u32>(AlphaMode::Blend);
      const u32 effective_pipeline = is_blend ? blend_pipeline_id : pipeline_id;

      const u64 key = (static_cast<u64>(effective_pipeline) << 48) |
                      (static_cast<u64>(resolved_mat) << 32) |
                      static_cast<u64>(prim.flat_index & 0xFFFF'FFFFU);

      frame_submission.entries.push_back({
          .sort_key = key,
          .mesh_prim_flat_index = prim.flat_index,
          .material_id = resolved_mat,
          .pipeline_id = effective_pipeline,
          .transform = node_t,
          .aabb = prim.aabb,
          .flags = gpu_mat.flags,
      });
    }
  }
}

void SceneRenderer::submit(MeshAssetHandle handle, const glm::mat4 &t,
                           std::span<const glm::mat4> joint_palette,
                           u32 pipeline_id, u32 material_id,
                           u32 blend_pipeline_id) {
  auto *asset = get_mesh(handle);
  if (asset == nullptr) [[unlikely]] {
    error("[Renderer] submit(skinned): null mesh asset for handle={}",
          handle.index());
    return;
  }

  for (const auto &node : asset->nodes) {
    const glm::mat4 node_t = t * node.local_transform;

    // Upload joint palette once per node (all primitives share the same skin).
    u32 palette_offset = ~0U;
    if (node.skin_index >= 0 && !joint_palette.empty()) {
      palette_offset = frame_palette_mat_count;
      pending_palette_data.insert(pending_palette_data.end(),
                                  joint_palette.begin(), joint_palette.end());
      frame_palette_mat_count += static_cast<u32>(joint_palette.size());
    }

    for (const auto &prim : node.primitives) {
      if (prim.flat_index >= static_cast<u32>(flat_prim_table.size()))
          [[unlikely]] {
        error("[Renderer] submit(skinned): flat_index {} out of bounds "
              "(table={}), handle={}",
              prim.flat_index, flat_prim_table.size(), handle.index());
        continue;
      }
      const u32 resolved_mat =
          material_id != ~0u ? material_id : prim.material_id;

      const auto &gpu_mat = geometry_pool->get_material(resolved_mat);
      const bool is_blend =
          blend_pipeline_id != ~0u &&
          gpu_mat.alpha_mode == static_cast<u32>(AlphaMode::Blend);
      const u32 effective_pipeline = is_blend ? blend_pipeline_id : pipeline_id;

      const u64 key = (static_cast<u64>(effective_pipeline) << 48) |
                      (static_cast<u64>(resolved_mat) << 32) |
                      static_cast<u64>(prim.flat_index & 0xFFFF'FFFFU);

      u32 skinned_base = ~0u;
      if (prim.lod_group.is_skinned() && palette_offset != ~0u) {
        skinned_base = frame_skin_dst_vertex;
        pending_skin_jobs.push_back({
            .src_vertex_offset = static_cast<u32>(prim.lod_group.vertex_offset),
            .skin_vertex_offset =
                static_cast<u32>(prim.lod_group.skin_vertex_offset),
            .dst_vertex_offset = frame_skin_dst_vertex,
            .vertex_count = prim.lod_group.vertex_count,
            .joint_palette_offset = palette_offset,
        });
        frame_skin_dst_vertex += prim.lod_group.vertex_count;
      }

      frame_submission.entries.push_back({
          .sort_key = key,
          .mesh_prim_flat_index = prim.flat_index,
          .material_id = resolved_mat,
          .pipeline_id = effective_pipeline,
          .transform = node_t,
          .aabb = prim.aabb,
          .flags = gpu_mat.flags,
          .skinned_base = skinned_base,
      });
    }
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
} // namespace

void SceneRenderer::ensure_global_capacity(usize instance_count) {
  ZoneScopedNC("ensure_global_capacity", 0x708090);
  auto &current = global_instance_buffer[current_frame_index];
  if (const auto size = instance_count * sizeof(CompressedInstanceData);
      !current || current->size() < size) {
    if (current) {
      DeletionQueue::the().push(
          [b = current->get_buffer(), a = current->get_allocation(),
           alloc = ctx.allocator] { vmaDestroyBuffer(alloc, b, a); });
      current->detach();
    }
    current = Buffer::create(ctx.allocator, "global_instance_buffer", size,
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  }
}

auto SceneRenderer::prepare(const FrameRenderInfo &info) -> PrepareResult {
  TracyVkCollectHost(tracy_vk_ctx->ctx);

  ZoneScopedNC("SceneRenderer::prepare", 0xFF00FF);
  assert(info.point_lights.size() <= FrameUBO::max_point_lights &&
         "too many point lights");

  {
    ZoneScopedNC("Poll Registries", 0x00FF7F);
    texture_upload_pool->poll_n(*this, 5);
    pipeline_registry->poll_and_update_dirty_pipelines();
  }

  {
    ZoneScopedNC("Process Pending HDR Map", 0x00FF7F);
    if (pending_hdr_map) [[unlikely]]
      process_pending_hdr_map();
  }

  current_frame_index = info.frame_index;

  if (override_pool.needs_grow) [[unlikely]] {
    ZoneScopedNC("Grow Pool", 0xFF4500);
    const u32 delta = grow_pool(*this);
    frame_submission.reset(0);
    // No bake this frame: drop stale batches and instance data so the per-frame
    // indirect buffers (which were not resized) are never drawn against.
    depth_prepass.batches.clear();
    forward_pass.batches.clear();
    global_instance_data.clear();
    // Drop skinning state for the same reason: palette buffer is not allocated
    // for the new frame index yet, and skinning_pass() is called
    // unconditionally.
    frame_skin_dst_vertex = 0;
    frame_palette_mat_count = 0;
    pending_palette_data.clear();
    pending_skin_jobs.clear();
    return {
        .status = PrepareResult::Status::SuccessMaterialPoolGrew,
        .material_pool_delta = delta,
    };
  }

  // Reset skinning accumulators; upload any palette data queued by submit().
  {
    const auto total_skin_verts = std::exchange(frame_skin_dst_vertex, 0u);
    const auto total_palette_mats = std::exchange(frame_palette_mat_count, 0u);
    if (total_skin_verts > 0)
      ensure_skinned_scratch(total_skin_verts);
    if (!pending_palette_data.empty()) {
      ensure_joint_palette_capacity(total_palette_mats);
      joint_palette_buffers[current_frame_index]->upload(pending_palette_data);
      pending_palette_data.clear();
    }
  }

  auto &fs = frame_submission;
  if (fs.entries.empty()) {
    depth_prepass.batches.clear();
    forward_pass.batches.clear();
    return {
        .status = PrepareResult::Status::SuccessNoSubmissions,
    };
  }

  {
    ZoneScopedNC("Sort Submissions", 0x1E90FF);
    fs.sort_order.resize(fs.entries.size());
    std::ranges::iota(fs.sort_order, 0u);
    std::sort(std::execution::par_unseq, fs.sort_order.begin(),
              fs.sort_order.end(), [&](u32 a, u32 b) {
                return fs.entries[a].sort_key < fs.entries[b].sort_key;
              });
  }

  {
    ZoneScopedNC("Build & Upload Instances", 0xADFF2F);
    ensure_global_capacity(fs.entries.size());
    global_instance_data.resize(fs.entries.size());
    for (u32 i = 0; i < static_cast<u32>(fs.sort_order.size()); ++i) {
      const auto &e = fs.entries[fs.sort_order[i]];
      if (e.mesh_prim_flat_index >= static_cast<u32>(flat_prim_table.size()))
          [[unlikely]] {
        error("[Renderer] prepare(): mesh_prim_flat_index {} out of bounds "
              "(table={}), entry skipped",
              e.mesh_prim_flat_index, flat_prim_table.size());
        global_instance_data[i] = {};
        continue;
      }
      const auto *lod_group = flat_prim_table[e.mesh_prim_flat_index].lod_group;
      if (lod_group == nullptr) [[unlikely]] {
        error("[Renderer] prepare(): null lod_group at flat_index {}, entry "
              "skipped",
              e.mesh_prim_flat_index);
        global_instance_data[i] = {};
        continue;
      }
      const auto mn = e.aabb.get_min();
      const auto mx = e.aabb.get_max();
      // Sphere centered at local origin — radius = max distance to any AABB
      // corner.
      const float bounding_radius = glm::length(glm::vec3{
          std::max(std::abs(mn.x), std::abs(mx.x)),
          std::max(std::abs(mn.y), std::abs(mx.y)),
          std::max(std::abs(mn.z), std::abs(mx.z)),
      });
      global_instance_data[i] = CompressedInstanceData{
          e.transform,
          static_cast<u16>(e.material_id),
          bounding_radius,
          lod_group->lod_count,
      };
      if (e.skinned_base != ~0u) {
        global_instance_data[i].padding0 = std::bit_cast<float>(e.skinned_base);
        global_instance_data[i].padding1 =
            std::bit_cast<float>(static_cast<u32>(lod_group->vertex_offset));
      }
    }
    global_instance_buffer[current_frame_index]->upload(global_instance_data);
  }

  {
    ZoneScopedNC("Compute & Upload UBO", 0xFFFF00);
    const glm::mat4 inv_view = glm::inverse(info.view);
    const glm::mat4 inv_proj = glm::inverse(info.projection);
    const glm::mat4 view_proj = info.projection * info.view;
    FrameUBO ubo{
        .view = info.view,
        .projection = info.projection,
        .view_projection = view_proj,
        .inverse_projection = inv_proj,
        .inverse_view = inv_view,
        .inverse_view_projection = glm::inverse(view_proj),
        .cascades = csm_frame_data.cascades,
        .frustum_planes = extract_frustum_planes(view_proj),
        .camera_position = inv_view[3],
        .sun_direction = sun_direction,
        .camera_near = info.camera_near_far.x, // you'll need to pass these in
        .camera_far = info.camera_near_far.y,
        .shadow_near = info.shadow_near_far.x,
        .shadow_far = info.shadow_near_far.y,
        .shadow_array_index = csm_frame_data.shadow_array_index,
        .shadow_sampler_index = csm_frame_data.shadow_sampler_index,
        .ibl_irradiance_index = ibl_probe.irradiance.index(),
        .ibl_prefiltered_index = ibl_probe.prefiltered.index(),
        .ibl_brdf_lut_index = ibl_probe.brdf_lut.index(),
        .ibl_sampler_index = dummy_sampler_handle.index(),
        .ibl_prefiltered_mips = ibl_probe.prefiltered_mip_count,
    };
    ubo.point_light_count = static_cast<u32>(info.point_lights.size());
    std::copy_n(info.point_lights.data(), info.point_lights.size(),
                ubo.point_lights.data());
    ubo.viewport_width = info.viewport_size.x;
    ubo.viewport_height = info.viewport_size.y;
    frame_ubo_buffers.at(info.frame_index)->upload(std::span(&ubo, 1));
  }

  const usize submission_hint = fs.entries.size();
  TracyPlot("draw_submissions", static_cast<i64>(submission_hint));

  {
    ZoneScopedNC("Bake Depth Prepass", 0x00FFFF);
    depth_prepass.bake(fs.sort_order, fs.entries, fs.entries.size());
  }

  {
    ZoneScopedNC("Bake Forward Pass", 0xFF69B4);
    forward_pass.bake(fs.sort_order, fs.entries, fs.entries.size());
  }

  fs.reset(submission_hint);
  return {.status = PrepareResult::Status::Success};
}

void SceneRenderer::render_pass(VkCommandBuffer cmd, RenderPass &pass,
                                VkPipeline override_pipeline) {
  ZoneScopedNC("SceneRenderer::render_pass", 0x32CD32);
  TracyVkZoneC(tracy_vk_ctx->ctx, cmd, "render_pass", 0x32CD32);

  auto &pool = *geometry_pool;
  if (pass.batches.empty())
    return;

  auto &ws = pass.frame_workspaces.at(current_frame_index);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout,
                          0U, 1U, &bindless.set, 0U, nullptr);
  vkCmdBindIndexBuffer(cmd, geometry_pool->index_buffer->get_buffer(), 0U,
                       VK_INDEX_TYPE_UINT32);

  const auto fi = static_cast<u32>(current_frame_index);
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
              global_instance_buffer[current_frame_index]->get_device_address(),
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
      .skinned_vertex_buffer_ptr =
          skinned_vertex_scratch[fi]
              ? skinned_vertex_scratch[fi]->get_device_address()
              : DeviceAddress::Invalid,
      .skinned_position_buffer_ptr =
          skinned_position_scratch[fi]
              ? skinned_position_scratch[fi]->get_device_address()
              : DeviceAddress::Invalid,
      .cluster_list_ptr =
          DeviceAddress{
              cluster_list_buffers[fi]->get_device_address(),
          },
      .light_list_ptr =
          DeviceAddress{
              light_list_buffers[fi]->get_device_address(),
          },
  };

  for (const auto &batch : pass.batches) {
    const VkPipeline pipe =
        override_pipeline != VK_NULL_HANDLE
            ? override_pipeline
            : pipeline_registry->get_unsafe(batch.pipeline_id);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    if (batch.depth_compare != VK_COMPARE_OP_MAX_ENUM)
      vkCmdSetDepthCompareOp(cmd, batch.depth_compare);
    vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_ALL, 0u,
                       sizeof(GpuPushConstants), &push_constants);

    vkCmdDrawIndexedIndirect(
        cmd, ws.indirect_buffer->get_buffer(),
        batch.first_command_index * sizeof(PaddedDrawCommand),
        batch.max_command_count, sizeof(PaddedDrawCommand));
  }
}

void SceneRenderer::depth_frustum_culling_pass(VkCommandBuffer cmd) {
  ZoneScopedNC("SceneRenderer::depth_frustum_culling_pass", 0x00BFFF);
  TracyVkZoneC(tracy_vk_ctx->ctx, cmd, "depth_frustum_culling_pass", 0x00BFFF);

  auto geometry_count = global_instance_data.size();
  if (geometry_count == 0 || depth_prepass.batches.empty() ||
      forward_pass.batches.empty())
    return;
  auto &depth_ws = depth_prepass.frame_workspaces.at(current_frame_index);

  auto make_host_to_compute = [](VkBuffer buf) -> VkBufferMemoryBarrier2 {
    return {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT, // upload_with_offset is a
                                                      // mapped write
        .srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
        .buffer = buf,
        .size = VK_WHOLE_SIZE,
    };
  };
  std::array<VkBufferMemoryBarrier2, 2> clear_barriers = {
      make_host_to_compute(depth_ws.indirect_buffer->get_buffer()),
      make_host_to_compute(depth_ws.instance_to_command_buffer->get_buffer()),
  };

  VkDependencyInfo clear_dep{
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .bufferMemoryBarrierCount = static_cast<u32>(clear_barriers.size()),
      .pBufferMemoryBarriers = clear_barriers.data(),
  };
  vkCmdPipelineBarrier2(cmd, &clear_dep);

  CullingPushConstants push{
      .instance_buffer =
          global_instance_buffer[current_frame_index]->get_device_address(),
      .frame_data =
          frame_ubo_buffers.at(current_frame_index)->get_device_address(),

      .depth_original_remap_buffer =
          depth_ws.index_remapping_buffer->get_device_address(),
      .depth_instance_to_command_buffer =
          depth_ws.instance_to_command_buffer->get_device_address(),
      .depth_indirect_commands = depth_ws.indirect_buffer->get_device_address(),
      .depth_culled_remap =
          depth_ws.culled_index_remapping_buffer->get_device_address(),

      .total_instance_count = static_cast<u32>(geometry_count),
  };

  const auto &entry = pipeline_registry->get_entry(depth_only_culling_pipeline);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, entry.pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, entry.layout, 0U,
                          1U, &bindless.set, 0U, nullptr);
  vkCmdPushConstants(cmd, entry.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0U,
                     sizeof(push), &push);

  auto dispatch_count = (geometry_count + 63) / 64;
  vkCmdDispatch(cmd, static_cast<u32>(dispatch_count), 1, 1);

  std::array<VkBufferMemoryBarrier2, 2> post_cull_barriers = {
      VkBufferMemoryBarrier2{
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
          .dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
          .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
          .buffer = depth_ws.culled_index_remapping_buffer->get_buffer(),
          .size = VK_WHOLE_SIZE,
      },
  };

  VkDependencyInfo post_dep{
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .bufferMemoryBarrierCount = static_cast<u32>(post_cull_barriers.size()),
      .pBufferMemoryBarriers = post_cull_barriers.data(),
  };
  vkCmdPipelineBarrier2(cmd, &post_dep);
}

auto SceneRenderer::blit_depth_to_pre_hiz_pass(VkCommandBuffer cmd,
                                               TextureHandle depth_resolved,
                                               TextureHandle depth_pre_hiz)
    -> void {
  ZoneScopedNC("SceneRenderer::blit_depth_to_pre_hiz_pass", 0x7B68EE);
  TracyVkZoneC(tracy_vk_ctx->ctx, cmd, "blit_depth_to_pre_hiz_pass", 0x7B68EE);

  const auto &src = textures.get(depth_resolved);
  const auto &dst = textures.get(depth_pre_hiz);
  const auto &pipeline_entry =
      pipeline_registry->get_entry(depth_to_r32_pipeline);

  const u32 w = src->texture.extent.width;
  const u32 h = src->texture.extent.height;

  const std::array<VkImageMemoryBarrier2, 2> pre_barriers{{
      {
          .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
          .srcStageMask = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
          .srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
          .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
          .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
          .newLayout = VK_IMAGE_LAYOUT_GENERAL,
          .image = src->texture.image,
          .subresourceRange =
              {
                  .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                  .levelCount = 1,
                  .layerCount = 1,
              },
      },
      {
          .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
          // WAR: pyramid build (COMPUTE READ) must finish before we overwrite.
          // srcAccessMask=NONE is correct for WAR — execution dep is enough.
          .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
          .srcAccessMask = VK_ACCESS_2_NONE,
          .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
          .dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
          .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
          .newLayout = VK_IMAGE_LAYOUT_GENERAL,
          .image = dst->texture.image,
          .subresourceRange =
              {
                  .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                  .levelCount = 1,
                  .layerCount = 1,
              },
      },
  }};

  VkDependencyInfo pre_dep{
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = static_cast<u32>(pre_barriers.size()),
      .pImageMemoryBarriers = pre_barriers.data(),
  };
  vkCmdPipelineBarrier2(cmd, &pre_dep);

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    pipeline_entry.pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                          pipeline_entry.layout, 0u, 1u, &bindless.set, 0u,
                          nullptr);

  struct {
    u32 src_depth_idx;
    u32 dst_color_idx;
    VkExtent2D src_dimension;
  } pc{
      .src_depth_idx = depth_resolved.index(),
      .dst_color_idx = depth_pre_hiz.index(),
      .src_dimension = {.width = w, .height = h},
  };
  vkCmdPushConstants(cmd, pipeline_entry.layout, VK_SHADER_STAGE_COMPUTE_BIT,
                     0u, sizeof(pc), &pc);

  vkCmdDispatch(cmd, (w + 15) / 16, (h + 15) / 16, 1);

  VkImageMemoryBarrier2 post_barrier{
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
      .newLayout = VK_IMAGE_LAYOUT_GENERAL,
      .image = dst->texture.image,
      .subresourceRange =
          {
              .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
              .levelCount = 1,
              .layerCount = 1,
          },
  };

  VkDependencyInfo post_dep{
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = 1,
      .pImageMemoryBarriers = &post_barrier,
  };
  vkCmdPipelineBarrier2(cmd, &post_dep);
}

void SceneRenderer::build_hierarchical_depth_pyramid_pass(
    VkCommandBuffer cmd, TextureHandle input_depth_image,
    TextureHandle output_pyramid) {
  ZoneScopedNC("SceneRenderer::build_hiz_pyramid", 0xDDA0DD);
  TracyVkZoneC(tracy_vk_ctx->ctx, cmd, "build_hiz_pyramid", 0xDDA0DD);

  const auto &resolved_depth = textures.get(input_depth_image);
  const auto &hiz_target = textures.get(output_pyramid);
  const auto &pipeline_entry =
      pipeline_registry->get_entry(hiz_downsample_pipeline);

  VkImageMemoryBarrier2 input_depth_barrier{
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
      .newLayout = VK_IMAGE_LAYOUT_GENERAL,
      .image = resolved_depth->texture.image,
      .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                           .levelCount = 1,
                           .layerCount = 1}};

  VkDependencyInfo init_dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                            .imageMemoryBarrierCount = 1,
                            .pImageMemoryBarriers = &input_depth_barrier};
  vkCmdPipelineBarrier2(cmd, &init_dep);

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    pipeline_entry.pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                          pipeline_entry.layout, 0U, 1U, &bindless.set, 0U,
                          nullptr);

  u32 mip_levels = hiz_target->texture.mip_levels;
  VkExtent2D current_src_extent = {
      .width = resolved_depth->texture.extent.width,
      .height = resolved_depth->texture.extent.height,
  };

  for (u32 mip = 0; mip < mip_levels; ++mip) {
    ZoneScopedNC("HiZ Mip Downsample", 0xDDA0DD);

    VkExtent2D current_dst_extent = {
        .width = std::max(1U, (current_src_extent.width + 1) / 2),
        .height = std::max(1U, (current_src_extent.height + 1) / 2),
    };

    HizPushConstants pc{
        .src_texture_idx =
            (mip == 0)
                ? input_depth_image.index()
                : hiz_target->texture.mip_layer_handle(mip - 1, 0).index(),
        .dst_texture_idx = hiz_target->texture.mip_layer_handle(mip, 0).index(),
        .src_dimension = current_src_extent,
    };
    vkCmdPushConstants(cmd, pipeline_entry.layout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0U, sizeof(pc), &pc);

    u32 dispatch_x = (current_dst_extent.width + 15) / 16;
    u32 dispatch_y = (current_dst_extent.height + 15) / 16;
    vkCmdDispatch(cmd, dispatch_x, dispatch_y, 1);

    VkImageMemoryBarrier2 mip_sync_barrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .image = hiz_target->texture.image,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .baseMipLevel = mip,
                             .levelCount = 1,
                             .layerCount = 1}};

    VkDependencyInfo mip_dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                             .imageMemoryBarrierCount = 1,
                             .pImageMemoryBarriers = &mip_sync_barrier};
    vkCmdPipelineBarrier2(cmd, &mip_dep);

    current_src_extent = current_dst_extent;
  }
}

void SceneRenderer::forward_occlusion_culling_pass(
    VkCommandBuffer cmd, const TextureHandle hiz_target) {
  ZoneScopedNC("SceneRenderer::forward_occlusion_culling_pass", 0xFF8C00);
  TracyVkZoneC(tracy_vk_ctx->ctx, cmd, "forward_occlusion_culling_pass",
               0xFF8C00);

  auto geometry_count = global_instance_data.size();
  auto &forward_ws = forward_pass.frame_workspaces.at(current_frame_index);

  if (geometry_count == 0)
    return;

  reset_indirect_counts(cmd, forward_pass);

  auto make_host_to_compute = [](VkBuffer buf) -> VkBufferMemoryBarrier2 {
    return {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT, // upload_with_offset is a
                                                      // mapped write
        .srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
        .buffer = buf,
        .size = VK_WHOLE_SIZE,
    };
  };
  std::array<VkBufferMemoryBarrier2, 2> clear_barriers = {
      make_host_to_compute(forward_ws.indirect_buffer->get_buffer()),
      make_host_to_compute(forward_ws.instance_to_command_buffer->get_buffer()),
  };
  VkDependencyInfo clear_dep{
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .bufferMemoryBarrierCount = static_cast<u32>(clear_barriers.size()),
      .pBufferMemoryBarriers = clear_barriers.data(),
  };
  vkCmdPipelineBarrier2(cmd, &clear_dep);

  const auto &resolved_pyramid = textures.get(hiz_target);
  const u32 mip_levels = resolved_pyramid->texture.mip_levels;

  OcclusionCullingPushConstants push{
      .instance_buffer =
          global_instance_buffer[current_frame_index]->get_device_address(),
      .frame_data =
          frame_ubo_buffers.at(current_frame_index)->get_device_address(),
      .forward_instance_to_command_buffer =
          forward_ws.instance_to_command_buffer->get_device_address(),
      .forward_indirect_commands =
          forward_ws.indirect_buffer->get_device_address(),
      .forward_culled_remap =
          forward_ws.culled_index_remapping_buffer->get_device_address(),
      .total_instance_count = static_cast<u32>(geometry_count),
      .hiz_sampler_idx = hiz_sampler_handle.index(),
      .hiz_width = static_cast<f32>(resolved_pyramid->texture.extent.width),
      .hiz_height = static_cast<f32>(resolved_pyramid->texture.extent.height),
  };

  // Flat index m maps to [m / 4][m % 4]; slot 15 carries the mip count.
  // The packing must mirror the shader's uint4[4] layout exactly.
  for (u32 m = 0; m < 15; ++m) {
    push.hiz_mip_indices[m / 4][m % 4] =
        (m < mip_levels) ? resolved_pyramid->texture.mip_layer_handle(m).index()
                         : 0U;
  }
  push.hiz_mip_indices[15 / 4][15 % 4] = mip_levels;

  const auto &entry = pipeline_registry->get_entry(forward_occlusion_pipeline);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, entry.pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, entry.layout, 0U,
                          1U, &bindless.set, 0U, nullptr);
  vkCmdPushConstants(cmd, entry.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0U,
                     sizeof(push), &push);

  auto dispatch_count = (geometry_count + 63) / 64;
  vkCmdDispatch(cmd, static_cast<u32>(dispatch_count), 1, 1);

  std::array<VkBufferMemoryBarrier2, 2> post_cull_barriers = {
      VkBufferMemoryBarrier2{
          .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
          .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
          .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
          .dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
          .buffer = forward_ws.indirect_buffer->get_buffer(),
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

  VkDependencyInfo post_dep{
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .bufferMemoryBarrierCount = 2U,
      .pBufferMemoryBarriers = post_cull_barriers.data(),
  };
  vkCmdPipelineBarrier2(cmd, &post_dep);
}

void SceneRenderer::light_clustering_pass(VkCommandBuffer cmd) {
  ZoneScopedNC("SceneRenderer::light_clustering_pass", 0xFFD700);
  TracyVkZoneC(tracy_vk_ctx->ctx, cmd, "light_clustering_pass", 0xFFD700);

  const auto fi = static_cast<u32>(current_frame_index);

  // Reset the allocation counter via the existing compute reset shader
  reset_field<u32>(cmd, *light_list_counter_buffers[fi], 1U, 0U, 0U);

  // Barrier: counter COMPUTE_WRITE -> COMPUTE_READ/WRITE for the clustering
  // pass
  const VkBufferMemoryBarrier2 counter_barrier{
      .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      .dstAccessMask =
          VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
      .buffer = light_list_counter_buffers[fi]->get_buffer(),
      .size = VK_WHOLE_SIZE,
  };
  const VkDependencyInfo counter_dep{
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .bufferMemoryBarrierCount = 1U,
      .pBufferMemoryBarriers = &counter_barrier,
  };
  vkCmdPipelineBarrier2(cmd, &counter_dep);

  const LightClusteringPushConstants push{
      .frame_data = frame_ubo_buffers.at(fi)->get_device_address(),
      .cluster_list = cluster_list_buffers[fi]->get_device_address(),
      .light_list = light_list_buffers[fi]->get_device_address(),
      .light_list_counter =
          light_list_counter_buffers[fi]->get_device_address(),
  };

  const auto &entry = pipeline_registry->get_entry(light_clustering_pipeline);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, entry.pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, entry.layout, 0U,
                          1U, &bindless.set, 0U, nullptr);
  vkCmdPushConstants(cmd, entry.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0U,
                     sizeof(push), &push);
  vkCmdDispatch(cmd, cluster_total, 1U, 1U);

  // Barrier: cluster data COMPUTE_WRITE -> FRAGMENT_READ
  std::array<VkBufferMemoryBarrier2, 2> post_cluster_barriers = {{
      {
          .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
          .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
          .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
          .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
          .buffer = cluster_list_buffers[fi]->get_buffer(),
          .size = VK_WHOLE_SIZE,
      },
      {
          .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
          .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
          .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
          .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
          .buffer = light_list_buffers[fi]->get_buffer(),
          .size = VK_WHOLE_SIZE,
      },
  }};
  const VkDependencyInfo post_dep{
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .bufferMemoryBarrierCount =
          static_cast<u32>(post_cluster_barriers.size()),
      .pBufferMemoryBarriers = post_cluster_barriers.data(),
  };
  vkCmdPipelineBarrier2(cmd, &post_dep);
}

void SceneRenderer::skybox_pass(VkCommandBuffer cmd) {
  ZoneScopedNC("SceneRenderer::skybox_pass", 0x87CEEB);
  TracyVkZoneC(tracy_vk_ctx->ctx, cmd, "skybox_pass", 0x87CEEB);

  const auto &entry = pipeline_registry->get_entry(skybox_pipeline);

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, entry.pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, entry.layout,
                          0u, 1u, &bindless.set, 0u, nullptr);

  struct SkyboxPushConstants {
    u32 env_map_index;
    u32 sampler_index;
    glm::mat4 inv_view_proj;
    f32 skybox_lod;
  };

  const auto frame_data =
      frame_ubo_buffers.at(current_frame_index)->read<FrameUBO>();
  const auto inv_view_projection_no_translation = glm::inverse(
      frame_data.projection * glm::mat4(glm::mat3(frame_data.view)));

  const SkyboxPushConstants push{
      .env_map_index = ibl_probe.prefiltered.index(),
      .sampler_index = cube_sampler_handle.index(),
      .inv_view_proj = inv_view_projection_no_translation,
      .skybox_lod = skybox_lod,
  };

  vkCmdPushConstants(cmd, entry.layout,
                     VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                     0u, sizeof(push), &push);
  vkCmdDraw(cmd, 3u, 1u, 0u, 0u);
}

void SceneRenderer::bloom_pass(VkCommandBuffer cmd) {
  ZoneScopedNC("SceneRenderer::bloom_pass", 0xFF69B4);
  TracyVkZoneC(tracy_vk_ctx->ctx, cmd, "bloom_pass", 0xFF69B4);

  if (!bloom_enabled || !bloom_chain_handle.valid())
    return;

  const auto *bloom_entry = textures.get(bloom_chain_handle);
  const u32 mip_count = bloom_entry->texture.mip_levels;
  const u32 sampler_idx = hiz_sampler_handle.index();

  auto mip_extent = [&](u32 mip) -> glm::uvec2 {
    return {
        std::max(1U, bloom_entry->texture.extent.width >> mip),
        std::max(1U, bloom_entry->texture.extent.height >> mip),
    };
  };

  auto compute_barrier = [&](u32 base_mip, u32 level_count) {
    const VkImageMemoryBarrier2 barrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask =
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .image = bloom_entry->texture.image,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .baseMipLevel = base_mip,
                             .levelCount = level_count,
                             .layerCount = 1},
    };
    const VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                               .imageMemoryBarrierCount = 1,
                               .pImageMemoryBarriers = &barrier};
    vkCmdPipelineBarrier2(cmd, &dep);
  };

  // ---- Downsample chain ----
  {
    const auto &entry = pipeline_registry->get_entry(bloom_downsample_pipeline);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, entry.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, entry.layout,
                            0U, 1U, &bindless.set, 0U, nullptr);

    // First pass: forward_target → bloom mip 0
    {
      const glm::uvec2 dst = mip_extent(0);
      const glm::uvec2 src = dst * 2U; // forward_target is 2× bloom mip 0
      const BloomDownsamplePushConstants pc{
          .src_texture_idx = forward_target_handle.index(),
          .dst_texture_idx =
              bloom_entry->texture.mip_layer_handle(0, 0).index(),
          .sampler_idx = sampler_idx,
          .is_first_pass = 1U,
          .src_size = src,
          .threshold = bloom_threshold,
      };
      vkCmdPushConstants(cmd, entry.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0U,
                         sizeof(pc), &pc);
      vkCmdDispatch(cmd, (dst.x + 7U) / 8U, (dst.y + 7U) / 8U, 1U);
      compute_barrier(0, 1);
    }

    // Subsequent passes: mip N-1 → mip N
    for (u32 mip = 1; mip < mip_count; ++mip) {
      const glm::uvec2 src = mip_extent(mip - 1);
      const glm::uvec2 dst = mip_extent(mip);
      const BloomDownsamplePushConstants pc{
          .src_texture_idx =
              bloom_entry->texture.mip_layer_handle(mip - 1, 0).index(),
          .dst_texture_idx =
              bloom_entry->texture.mip_layer_handle(mip, 0).index(),
          .sampler_idx = sampler_idx,
          .is_first_pass = 0U,
          .src_size = src,
          .threshold = bloom_threshold,
      };
      vkCmdPushConstants(cmd, entry.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0U,
                         sizeof(pc), &pc);
      vkCmdDispatch(cmd, (dst.x + 7U) / 8U, (dst.y + 7U) / 8U, 1U);
      compute_barrier(mip, 1);
    }
  }

  // ---- Separable upsample chain ----
  // For each level: H blur (bloom mip+1 → scratch mip), then V blur +
  // accumulate (scratch mip → bloom mip)
  {
    const auto *scratch_entry = textures.get(bloom_scratch_handle);

    const auto &entry = pipeline_registry->get_entry(bloom_blur_pipeline);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, entry.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, entry.layout,
                            0U, 1U, &bindless.set, 0U, nullptr);

    auto scratch_barrier = [&](u32 base_mip) {
      const VkImageMemoryBarrier2 barrier{
          .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
          .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
          .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
          .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
          .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
          .newLayout = VK_IMAGE_LAYOUT_GENERAL,
          .image = scratch_entry->texture.image,
          .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                               .baseMipLevel = base_mip,
                               .levelCount = 1,
                               .layerCount = 1},
      };
      const VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                 .imageMemoryBarrierCount = 1,
                                 .pImageMemoryBarriers = &barrier};
      vkCmdPipelineBarrier2(cmd, &dep);
    };

    for (i32 mip = static_cast<i32>(mip_count) - 2; mip >= 0; --mip) {
      const auto umip = static_cast<u32>(mip);
      const glm::uvec2 dst = mip_extent(umip);

      // H blur: bloom_chain mip+1 → scratch mip
      {
        const BloomBlurPushConstants pc{
            .src_texture_idx =
                bloom_entry->texture.mip_layer_handle(umip + 1, 0).index(),
            .dst_texture_idx =
                scratch_entry->texture.mip_layer_handle(umip, 0).index(),
            .sampler_idx = sampler_idx,
            .is_vertical = 0U,
            .dst_size = dst,
            .scatter = bloom_scatter,
            .accumulate = 0U,
        };
        vkCmdPushConstants(cmd, entry.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0U,
                           sizeof(pc), &pc);
        vkCmdDispatch(cmd, (dst.x + 7U) / 8U, (dst.y + 7U) / 8U, 1U);
        scratch_barrier(umip);
      }

      // V blur + accumulate: scratch mip → bloom_chain mip
      {
        const BloomBlurPushConstants pc{
            .src_texture_idx =
                scratch_entry->texture.mip_layer_handle(umip, 0).index(),
            .dst_texture_idx =
                bloom_entry->texture.mip_layer_handle(umip, 0).index(),
            .sampler_idx = sampler_idx,
            .is_vertical = 1U,
            .dst_size = dst,
            .scatter = bloom_scatter,
            .accumulate = 1U,
        };
        vkCmdPushConstants(cmd, entry.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0U,
                           sizeof(pc), &pc);
        vkCmdDispatch(cmd, (dst.x + 7U) / 8U, (dst.y + 7U) / 8U, 1U);
        compute_barrier(umip, 1);
      }
    }
  }

  // Bloom mip 0 compute write → fragment read (for composite)
  const VkImageMemoryBarrier2 bloom_to_composite{
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
      .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
      .newLayout = VK_IMAGE_LAYOUT_GENERAL,
      .image = bloom_entry->texture.image,
      .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                           .baseMipLevel = 0,
                           .levelCount = 1,
                           .layerCount = 1},
  };
  const VkDependencyInfo final_dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                   .imageMemoryBarrierCount = 1,
                                   .pImageMemoryBarriers = &bloom_to_composite};
  vkCmdPipelineBarrier2(cmd, &final_dep);
}

void SceneRenderer::composite_pass(VkCommandBuffer cmd) {
  ZoneScopedNC("SceneRenderer::composite_pass", 0xFFA07A);
  TracyVkZoneC(tracy_vk_ctx->ctx, cmd, "composite_pass", 0xFFA07A);

  const auto &entry = pipeline_registry->get_entry(composite_pipeline);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, entry.pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, entry.layout,
                          0u, 1u, &bindless.set, 0u, nullptr);
  const CompositePushConstants push{
      .forward_texture_index = forward_target_handle,
      .sampler = dummy_sampler_handle,
      .bloom_texture_index =
          bloom_enabled ? bloom_chain_handle : TextureHandle{},
      .bloom_strength = bloom_strength,
  };
  vkCmdPushConstants(cmd, entry.layout,
                     VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                     0u, sizeof(push), &push);
  vkCmdDraw(cmd, 3U, 1u, 0u, 0u);
}

auto RenderPass::ensure_capacity(usize command_count, usize instance_count,
                                 usize batch_count,
                                 usize total_global_instances) -> bool {
  ZoneScopedNC("RenderPass::ensure_capacity", 0x708090);
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
                           VkBufferUsageFlags flags, std::string_view name) {
    if (!buffer || buffer->size() < needed_size) {
      if (buffer) {
        vkDeviceWaitIdle(dev);
      }
      buffer = Buffer::create(alloc, name, needed_size, flags);
    }
  };

  ensure_buffer(ws.indirect_buffer, indirect_needed,
                indirect_flags | VK_BUFFER_USAGE_2_TRANSFER_DST_BIT,
                "Indirect Buffer");
  ensure_buffer(ws.count_buffer, count_needed, indirect_flags, "Count Buffer");
  ensure_buffer(ws.instance_to_command_buffer, global_needed, storage_flags,
                "Instance to Command Buffer");
  ensure_buffer(ws.index_remapping_buffer, instance_needed, storage_flags,
                "Index Remapping Buffer");
  ensure_buffer(ws.culled_index_remapping_buffer, instance_needed,
                storage_flags, "Culled Index Remapping Buffer");
  ensure_buffer(ws.max_instances_per_cmd_buffer, command_count * sizeof(u32),
                storage_flags, "Max Instances Per Cmd Buffer");

  return true;
}

auto RenderPass::bake(std::span<const u32> sorted_order,
                      std::span<const FrameSubmission::Entry> entries,
                      usize total_global_instances) -> void {
  ZoneScopedNC("RenderPass::bake", 0x00FFFF);

  std::vector<PaddedDrawCommand> commands;
  std::vector<u32> remapped_indices;
  std::vector<u32> draw_counts;
  std::vector<u32> max_instances;
  std::vector<u32> instance_to_commands(total_global_instances, 0xFFFF'FFFFu);
  batches.clear();
  u32 current_pipeline = ~0U;

  {
    ZoneScopedNC("Process Submissions", 0xFF8C00);
    usize i = 0;
    while (i < sorted_order.size()) {
      const u32 head_idx = sorted_order[i];
      const auto &head = entries[head_idx];

      if (head.pipeline_id != current_pipeline) {
        ZoneScopedNC("Switch Pipeline Batch", 0xFF00FF);
        current_pipeline = head.pipeline_id;
        draw_counts.push_back(0u);

        VkCompareOp depth_compare = VK_COMPARE_OP_MAX_ENUM;
        if (type == RenderPassType::Forward) {
          depth_compare = has_flag(head.flags, MaterialFlags::alpha_blend)
                              ? VK_COMPARE_OP_GREATER_OR_EQUAL
                              : VK_COMPARE_OP_EQUAL;
        }

        batches.push_back({
            .pipeline_id = head.pipeline_id,
            .max_command_count = 0u,
            .first_command_index = static_cast<u32>(commands.size()),
            .count_buffer_offset =
                static_cast<u32>((draw_counts.size() - 1) * sizeof(u32)),
            .depth_compare = depth_compare,
        });
      }

      const u64 run_key = head.sort_key;
      const u32 lod0_cmd = static_cast<u32>(commands.size());
      const usize run_start = i;

      {
        ZoneScopedNC("Find Run Group", 0x3CB371);
        while (i < sorted_order.size() &&
               entries[sorted_order[i]].sort_key == run_key)
          ++i;
      }

      // Alpha-masked geometry IS rendered in the depth prepass: depth.slang's
      // fragment shader discards texels below the cutoff, so the surviving
      // texels write depth and the forward pass's EQUAL test matches them. Only
      // blended / transmissive materials are excluded — they write no depth in
      // the prepass and the forward pass tests them with GREATER_OR_EQUAL.
      constexpr auto excluded_from_depth_prepass =
          MaterialFlags::alpha_blend | MaterialFlags::has_transmission;
      const bool in_depth_prepass =
          type != RenderPassType::DepthPrepass ||
          (head.flags & excluded_from_depth_prepass) == MaterialFlags::None;

      if (in_depth_prepass) {
        for (usize j = run_start; j < i; ++j)
          instance_to_commands[j] = lod0_cmd;
      }

      const FlatPrimitive &fp =
          renderer.flat_prim_table[head.mesh_prim_flat_index];

      {
        ZoneScopedNC("Generate LOD Commands", 0x1E90FF);
        for (u8 lod = 0; lod < fp.lod_group->lod_count; ++lod) {
          const u32 first_instance = static_cast<u32>(remapped_indices.size());

          for (usize j = run_start; j < i; ++j)
            remapped_indices.push_back(static_cast<u32>(j));

          const auto m = fp.lod_group->resolve(lod);
          commands.push_back({
              .index_count = m.index_count,
              .instance_count = 0u,
              .first_index = m.first_index,
              .vertex_offset = m.vertex_offset,
              .first_instance = first_instance,
          });
          max_instances.push_back(static_cast<u32>(i - run_start));

          batches.back().max_command_count++;
          draw_counts.back()++;
        }
      }
    }
  }

  {
    ZoneScopedNC("Ensure Allocation Capacity", 0x708090);
    ensure_capacity(commands.size(), remapped_indices.size(),
                    draw_counts.size(), total_global_instances);
  }

  TracyPlot("indirect_commands", static_cast<i64>(commands.size()));
  TracyPlot("draw_batches", static_cast<i64>(batches.size()));

  {
    ZoneScopedNC("Upload GPU Workspaces", 0xADFF2F);
    auto &ws = frame_workspaces.at(current_frame_index);
    ws.indirect_buffer->upload_with_offset(commands, 0);
    ws.count_buffer->upload_with_offset(draw_counts, 0);
    ws.index_remapping_buffer->upload_with_offset(remapped_indices, 0);
    ws.instance_to_command_buffer->upload_with_offset(instance_to_commands, 0);
    ws.max_instances_per_cmd_buffer->upload_with_offset(max_instances, 0);
  }
}

void SceneRenderer::render_shadow_cascade(VkCommandBuffer cmd,
                                          u32 cascade_idx) {
  ZoneScopedNC("SceneRenderer::render_shadow_cascade", 0xB8860B);
  TracyVkZoneC(tracy_vk_ctx->ctx, cmd, "render_shadow_cascade", 0xB8860B);

  auto &pass = depth_prepass;
  if (pass.batches.empty())
    return;

  auto &ws = pass.frame_workspaces.at(current_frame_index);

  auto &pool = *geometry_pool;
  const auto shadow_fi = static_cast<u32>(current_frame_index);
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
              global_instance_buffer[current_frame_index]->get_device_address(),
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
      .skinned_vertex_buffer_ptr =
          skinned_vertex_scratch[shadow_fi]
              ? skinned_vertex_scratch[shadow_fi]->get_device_address()
              : DeviceAddress::Invalid,
      .skinned_position_buffer_ptr =
          skinned_position_scratch[shadow_fi]
              ? skinned_position_scratch[shadow_fi]->get_device_address()
              : DeviceAddress::Invalid,
      .cascade_index = cascade_idx,
  };

  vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_ALL, 0U,
                     sizeof(GpuPushConstants), &push_constants);

  for (const auto &batch : pass.batches) {
    vkCmdDrawIndexedIndirect(
        cmd, ws.indirect_buffer->get_buffer(),
        batch.first_command_index * sizeof(PaddedDrawCommand),
        batch.max_command_count, sizeof(PaddedDrawCommand));
  }
}

void SceneRenderer::update_csm(const glm::mat4 &view, const glm::mat4 &proj,
                               float camera_near, float camera_far) {
  ZoneScopedNC("SceneRenderer::update_csm", 0xFFAA00);
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
  ZoneScopedNC("CsmResources::destroy", 0xFF0000);
  vmaDestroyImage(allocator, image, allocation);
  for (auto &v : layer_views)
    vkDestroyImageView(device, v, nullptr);
  vkDestroyImageView(device, array_view, nullptr);
}

void SceneRenderer::init_csm() {
  ZoneScopedNC("SceneRenderer::init_csm", 0xFFAA00);
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
    csm.layer_handles[i] =
        register_external_view(csm.layer_views[i], VK_IMAGE_VIEW_TYPE_2D);
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

  this->bounding_radius = bounding_radius;
  padding0 = std::bit_cast<float>(~0u); // sentinel: not skinned
  padding1 = 0.0f;
}

auto SceneRenderer::create_ibl_probe_from_hdr(SceneRenderer &renderer,
                                              const VFSPath &path)
    -> std::expected<IblProbe, std::string> {
  ZoneScopedNC("create_ibl_probe_from_hdr", 0xFFD700);

  auto equirect_tex = Texture::load_ktx2_hdr_texture(renderer.ctx, path);
  if (!equirect_tex) {
    return std::unexpected("Could not load texture");
  }

  const auto equirect = renderer.textures.create(TextureEntry{
      .texture = std::move(equirect_tex.value()),
      .sampled_view_type = VK_IMAGE_VIEW_TYPE_2D,
  });

  auto probe = IblProbe::create(renderer.ctx, renderer, equirect);
  if (!probe)
    return std::unexpected(probe.error());

  if (auto *entry = renderer.textures.get(equirect)) {
    entry->texture.destroy(renderer.ctx, &renderer.subimages);
    entry->texture.destroy(renderer.ctx, &renderer.textures);
  }
  renderer.textures.destroy(equirect);

  return probe;
}

auto SceneRenderer::set_hdr_map(VFSPath path) -> void {
  pending_hdr_map = std::move(path);
}

auto SceneRenderer::process_pending_hdr_map() -> void {
  breadcrumb("hdr_map_process");
  ZoneScopedNC("SceneRenderer::process_pending_hdr_map", 0xFFD700);
  auto hdr_map = std::move(pending_hdr_map.value());
  pending_hdr_map.reset();

  const auto ext = hdr_map.extension();
  const bool needs_conversion = (ext == ".hdr" || ext == ".exr");
  const bool already_ktx2 = (ext == ".ktx2");

  if (!needs_conversion && !already_ktx2) {
    error("Unsupported HDR map format '{}': expected .hdr, .exr, or .ktx2",
          ext);
    return;
  }

  VFSPath source_path = hdr_map;
  VFS::ScopedMount pending_mount;

  if (needs_conversion) {
    const auto physical = VFS::get().resolve(hdr_map);
    auto [scope, path] = VFS::get().mount_file_scoped("pending_hdr", physical);
    pending_mount = std::move(scope);
    source_path = path;
  }

  const auto ktx2_path = source_path.with_extension(".ktx2");

  if (needs_conversion) {
    auto result = convert_hdr_to_ktx2(source_path, ktx2_path, /*force=*/false);
    if (!result) {
      error("Failed to convert HDR map: {}", result.error());
      return;
    }
  }

  auto new_probe = SceneRenderer::create_ibl_probe_from_hdr(*this, ktx2_path);
  IblProbe old_probe = ibl_probe;
  if (new_probe) {
    old_probe = std::exchange(ibl_probe, std::move(new_probe.value()));
  } else {
    return;
  }

  vkDeviceWaitIdle(ctx.device);
  old_probe.destroy(ctx, *this);

  bindless.mark_dirty();
}

void SceneRenderer::reset_indirect_counts(VkCommandBuffer cmd,
                                          RenderPass &pass) {
  ZoneScopedNC("SceneRenderer::reset_indirect_counts", 0x708090);
  auto &ws = pass.frame_workspaces.at(current_frame_index);
  const u32 command_count = std::accumulate(
      pass.batches.begin(), pass.batches.end(), 0u,
      [](u32 acc, const auto &b) { return acc + b.max_command_count; });
  if (command_count == 0)
    return;

  reset_field<PaddedDrawCommand>(cmd, *ws.indirect_buffer, command_count,
                                 offsetof(PaddedDrawCommand, instance_count));

  VkBufferMemoryBarrier2 reset_barrier{
      .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      .dstAccessMask =
          VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
      .buffer = ws.indirect_buffer->get_buffer(),
      .size = VK_WHOLE_SIZE,
  };
  VkDependencyInfo reset_dep{
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .bufferMemoryBarrierCount = 1U,
      .pBufferMemoryBarriers = &reset_barrier,
  };
  vkCmdPipelineBarrier2(cmd, &reset_dep);
}

void SceneRenderer::ensure_skinned_scratch(usize vertex_count) {
  const u64 fi = current_frame_index;
  auto &vs = skinned_vertex_scratch[fi];
  auto &ps = skinned_position_scratch[fi];
  if (vs && vs->size() >= vertex_count * sizeof(Vertex) && ps &&
      ps->size() >= vertex_count * sizeof(PositionOnlyVertex))
    return;
  breadcrumb("skinned_scratch_alloc");
  const usize new_cap =
      std::max(vertex_count, (skinned_scratch_capacity * 3 / 2) + 1);
  if (vs) {
    DeletionQueue::the().push(
        [b = vs->get_buffer(), a = vs->get_allocation(),
         alloc = ctx.allocator] { vmaDestroyBuffer(alloc, b, a); });
    vs->detach();
  }
  if (ps) {
    DeletionQueue::the().push(
        [b = ps->get_buffer(), a = ps->get_allocation(),
         alloc = ctx.allocator] { vmaDestroyBuffer(alloc, b, a); });
    ps->detach();
  }
  vs = Buffer::create(ctx.allocator, "skinned_vertex_scratch",
                      new_cap * sizeof(Vertex),
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  ps = Buffer::create(ctx.allocator, "skinned_position_scratch",
                      new_cap * sizeof(PositionOnlyVertex),
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  skinned_scratch_capacity = new_cap;
}

void SceneRenderer::ensure_joint_palette_capacity(usize mat_count) {
  const u64 fi = current_frame_index;
  auto &buf = joint_palette_buffers[fi];
  const auto byte_size = mat_count * sizeof(glm::mat4);
  if (buf && buf->size() >= byte_size)
    return;
  breadcrumb("joint_palette_alloc");
  if (buf) {
    DeletionQueue::the().push(
        [b = buf->get_buffer(), a = buf->get_allocation(),
         alloc = ctx.allocator] { vmaDestroyBuffer(alloc, b, a); });
    buf->detach();
  }
  buf = Buffer::create(ctx.allocator, "joint_palette", byte_size,
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  joint_palette_capacity = std::max(joint_palette_capacity, mat_count);
}

void SceneRenderer::skinning_pass(VkCommandBuffer cmd) {
  ZoneScopedNC("SceneRenderer::skinning_pass", 0xFF8C00);
  TracyVkZoneC(tracy_vk_ctx->ctx, cmd, "skinning_pass", 0xFF8C00);

  if (pending_skin_jobs.empty())
    return;

  const auto fi = static_cast<u32>(current_frame_index);

  // Barrier: CPU palette upload → compute read
  VkBufferMemoryBarrier2 palette_bar{
      .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
      .srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
      .buffer = joint_palette_buffers[fi]->get_buffer(),
      .size = VK_WHOLE_SIZE,
  };
  VkDependencyInfo pre_dep{
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .bufferMemoryBarrierCount = 1U,
      .pBufferMemoryBarriers = &palette_bar,
  };
  vkCmdPipelineBarrier2(cmd, &pre_dep);

  const auto &entry = pipeline_registry->get_entry(skinning_pipeline);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, entry.pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, entry.layout, 0U,
                          1U, &bindless.set, 0U, nullptr);

  const auto src_v_addr = geometry_pool->vertex_buffer->get_device_address();
  const auto src_p_addr =
      geometry_pool->position_only_vertex_buffer->get_device_address();
  const auto skin_addr =
      geometry_pool->skin_vertex_buffer
          ? geometry_pool->skin_vertex_buffer->get_device_address()
          : DeviceAddress{};
  const auto pal_addr = joint_palette_buffers[fi]->get_device_address();
  const auto dst_v_addr = skinned_vertex_scratch[fi]->get_device_address();
  const auto dst_p_addr = skinned_position_scratch[fi]->get_device_address();

  for (const auto &job : pending_skin_jobs) {
    SkinningPushConstants pc{
        .src_vertices = src_v_addr,
        .src_positions = src_p_addr,
        .skin_attrs = skin_addr,
        .joint_palette = pal_addr,
        .dst_vertices = dst_v_addr,
        .dst_positions = dst_p_addr,
        .src_vertex_offset = job.src_vertex_offset,
        .skin_vertex_offset = job.skin_vertex_offset,
        .dst_vertex_offset = job.dst_vertex_offset,
        .vertex_count = job.vertex_count,
        .joint_palette_offset = job.joint_palette_offset,
    };
    vkCmdPushConstants(cmd, entry.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0U,
                       sizeof(pc), &pc);
    vkCmdDispatch(cmd, (job.vertex_count + 63U) / 64U, 1U, 1U);
  }

  // Barrier: compute write → vertex shader read
  VkMemoryBarrier2 post_bar{
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
      .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
  };
  VkDependencyInfo post_dep{
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .memoryBarrierCount = 1U,
      .pMemoryBarriers = &post_bar,
  };
  vkCmdPipelineBarrier2(cmd, &post_dep);

  pending_skin_jobs.clear();
}

} // namespace dy

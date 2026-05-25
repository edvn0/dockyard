#pragma once

#include "dockyard/texture.hpp"
#include <dockyard/app.hpp>
#include <dockyard/buffer.hpp>
#include <dockyard/compiler.hpp>
#include <dockyard/context.hpp>
#include <dockyard/device_geometry.hpp>
#include <dockyard/mesh.hpp>
#include <dockyard/mesh_loader.hpp>
#include <dockyard/pipeline_builder.hpp>
#include <dockyard/scene.hpp>
#include <dockyard/thread_safe_memory_cache.hpp>

#include <deque>
#include <glm/glm.hpp>
#include <type_traits>
#include <vulkan/vulkan_core.h>

namespace dy {

namespace shader {
class ShaderWatcher;
}

struct GpuPushConstants {
  const DeviceAddress vertex_buffer_ptr;
  const DeviceAddress position_only_buffer_ptr;
  const DeviceAddress transform_buffer_ptr;
  const DeviceAddress culled_index_remapping_buffer;
  const DeviceAddress frame_ubo;
  const DeviceAddress material_ptr;
  u32 cascade_index;
  u32 padding[3];
};

struct CompositePushConstants {
  const TextureHandle forward_texture_index;
  const SamplerHandle sampler;
};

struct CullingPushConstants {
  DeviceAddress instance_buffer;
  DeviceAddress frame_data;

  DeviceAddress depth_original_remap_buffer;
  DeviceAddress depth_instance_to_command_buffer;
  DeviceAddress depth_indirect_commands;
  DeviceAddress depth_culled_remap;

  DeviceAddress forward_original_remap_buffer;
  DeviceAddress forward_instance_to_command_buffer;
  DeviceAddress forward_indirect_commands;
  DeviceAddress forward_culled_remap;

  u32 total_instance_count;
  u32 padding[3];
};

enum class RenderPassType { DepthPrepass, Forward, Shadow };

struct PendingDraw {
  Mesh mesh;
  u32 pipeline_id;
  u32 material_id;
  glm::mat4 transform;
  AABB aabb;

  u32 instance_id;

  [[nodiscard]] constexpr auto get_key(RenderPassType pass) const -> u64;
};

struct RenderPass {
  RenderPassType type;
  VmaAllocator allocator;

  struct DrawBucket {
    Mesh mesh;
    u32 pipeline_id;
    u32 material_id;
    std::vector<u32> instance_ids; // indices into global_instance_data
  };
  std::map<u64, DrawBucket> buckets; // key = get_key() result, already ordered
  struct Batch {
    u32 pipeline_id;
    u32 max_command_count;
    u32 first_command_index;
    u32 count_buffer_offset;
  };
  std::vector<Batch> batches;
  struct FrameWorkspace {
    std::unique_ptr<Buffer> indirect_buffer;
    std::unique_ptr<Buffer> count_buffer;
    std::unique_ptr<Buffer> instance_to_command_buffer;
    std::unique_ptr<Buffer> index_remapping_buffer;
    std::unique_ptr<Buffer> culled_index_remapping_buffer;
  };
  std::array<FrameWorkspace, frames_in_flight> frame_workspaces{};

  auto ensure_capacity(usize command_count, usize instance_count,
                       usize batch_count, usize total_global_instances) -> bool;
  auto bake(usize) -> bool;
};

struct InstanceData {
  glm::mat4 transform;
  u32 material_id;
  float bounding_radius; // replaces pad0
  u32 pad1;
  u32 pad2;
};

struct alignas(16) CascadeData {
  glm::mat4 view_proj;
  float split_depth; // view-space z of cascade far (negative, RH)
  float _pad[3];
};

static constexpr auto shadow_map_cascade_count = 6;
static constexpr u32 shadow_map_cascade_resolution = 2048;

struct FrameUBO {
  glm::mat4 view;
  glm::mat4 projection;
  glm::mat4 view_projection;
  glm::mat4 inverse_projection;
  glm::mat4 inverse_view;
  glm::mat4 inverse_view_projection;
  std::array<CascadeData, shadow_map_cascade_count> cascades{};
  std::array<glm::vec4, 6> frustum_planes{};
  glm::vec4 camera_position;
  glm::vec4 sun_direction;
  u32 shadow_array_index;
  u32 shadow_sampler_index;
  u32 ibl_irradiance_index;  // binding 4 (sampled_cubemaps)
  u32 ibl_prefiltered_index; // binding 4 (sampled_cubemaps)
  u32 ibl_brdf_lut_index;    // binding 0 (sampled_images)
  u32 ibl_sampler_index;     // binding 1 (samplers) — linear + mip
  u32 ibl_prefiltered_mips;  // needed for roughness LOD selection
  u32 pad0;
  u32 pad1; // pad to 16-byte multiple
};
static_assert(sizeof(FrameUBO) % 16 == 0);

struct CsmResources {
  VkImage image = VK_NULL_HANDLE;
  VmaAllocation allocation = VK_NULL_HANDLE;
  VkImageView array_view = VK_NULL_HANDLE;
  std::array<VkImageView, shadow_map_cascade_resolution> layer_views{};

  TextureHandle bindless_handle;

  void destroy(VkDevice device, VmaAllocator allocator);
};

struct SceneRenderer {
  VulkanContext &ctx;
  SwapchainResources &swapchain;

  RenderPass depth_prepass;
  RenderPass forward_pass;

  TexturePool textures;
  SamplerPool samplers;
  ComparisonSamplerPool comparison_samplers;
  SubImagePool subimages;
  BindlessSet bindless;
  std::unique_ptr<GeometryPool> geometry_pool;

  std::deque<MeshAsset> mesh_registry;

  TextureHandle dummy_texture_handle;
  SamplerHandle dummy_sampler_handle;
  TextureHandle white_texture;
  TextureHandle normal_texture;
  TextureHandle metallic_roughness_texture;
  TextureHandle occlusion_texture;
  TextureHandle black_texture;

  TextureHandle forward_target_handle;
  auto update_output_texture(TextureHandle h) { forward_target_handle = h; }

  VkSampler dummy_sampler_vk = VK_NULL_HANDLE;
  VkSampler comparison_sampler_vk = VK_NULL_HANDLE;

  std::vector<InstanceData> global_instance_data{};
  std::vector<PendingDraw> submission_queue{};
  std::unique_ptr<Buffer> global_instance_buffer{nullptr};

  std::vector<std::unique_ptr<Buffer>> frame_ubo_buffers{};

  VkPipelineLayout pipeline_layout{VK_NULL_HANDLE};
  std::unique_ptr<PipelineRegistry> pipeline_registry{nullptr};
  CsmResources csm{};
  struct CsmFrameData {
    std::array<CascadeData, shadow_map_cascade_count> cascades{};
    u32 shadow_array_index = 0u;
    u32 shadow_sampler_index = 0u;
  } csm_frame_data;
  VkSampler shadow_comparison_sampler_vk = VK_NULL_HANDLE;
  u32 shadow_sampler_bindless_idx = 0u;

  IblProbe ibl_probe;

  std::array<CascadeData, shadow_map_cascade_count> cascade_cpu_data{};
  glm::vec4 sun_direction =
      glm::normalize(glm::vec4{0, 0, 0, 0} - glm::vec4{3, -7, 5, 0});

  PipelineHandle shadow_pipeline;
  PipelineHandle composite_pipeline;
  PipelineHandle skybox_pipeline;
  PipelineHandle culling_pipeline;

  Cache<StringMap<TextureHandle>> texture_cache{};

  // -----------------------------------------------------------------------

  explicit SceneRenderer(VulkanContext &c, SwapchainResources &sc);

  auto initialise_bindless() -> void;
  void init_csm();
  auto upload_texture(std::span<const u32> data, std::string_view name, u32 w,
                      u32 h, VkFormat fmt, bool gen_mips, bool storage = true)
      -> TextureHandle;

  auto resize() -> void;
  auto destroy() -> void;

  void ensure_global_capacity(usize instance_count);

  // Prepare should setup frame index.
  auto prepare(u64 frame_index, const glm::mat4 &view,
               const glm::mat4 &projection) -> bool;

  void submit(MeshHandle handle, const glm::mat4 &, u32 pipeline_id = 0U,
              u32 material_id = 0U);

  void update_csm(const glm::mat4 &view, const glm::mat4 &proj,
                  float camera_near, float camera_far);
  void render_shadow_cascade(VkCommandBuffer cmd, u32 cascade_idx);
  void render_pass(VkCommandBuffer, RenderPass &,
                   VkPipeline override_pipeline = VK_NULL_HANDLE);
  void composite_pass(VkCommandBuffer);
  void culling_pass(VkCommandBuffer);
  void skybox_pass(VkCommandBuffer);

  auto register_gltf(MeshAsset &&asset) -> MeshHandle;
  auto register_external_view(VkImageView view, VkImageViewType type)
      -> TextureHandle;
  auto get_mesh(MeshHandle handle) -> MeshAsset *;
  auto get_mesh(MeshHandle handle) const -> const MeshAsset *;
  [[nodiscard]] auto get_material_view(MeshHandle handle) const
      -> ConstMaterialView;
  [[nodiscard]] auto get_material_view_mut(MeshHandle handle)
      -> MutableMaterialView;
  [[nodiscard]] auto get_material_view(const MeshAsset &) const
      -> ConstMaterialView;
  [[nodiscard]] auto get_material_view_mut(const MeshAsset &) const
      -> MutableMaterialView;

  template <typename Handle>
  auto resolve(Handle handle) const -> decltype(auto) {
    if constexpr (std::is_same_v<Handle, TextureHandle>) {
      return textures.get(handle)->texture;
    } else if constexpr (std::is_same_v<Handle, SamplerHandle>) {
      return samplers.get(handle)->sampler;
    } else if constexpr (std::is_same_v<Handle, ComparisonSamplerHandle>) {
      return comparison_samplers.get(handle)->sampler;
    } else {
      static_assert(false, "Unsupported handle type"); // Valid in C++26!
    }
  }
};

} // namespace dy
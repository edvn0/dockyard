#pragma once

#include <deque>
#include <dockyard/app.hpp>
#include <dockyard/buffer.hpp>
#include <dockyard/compiler.hpp>
#include <dockyard/context.hpp>
#include <dockyard/device_geometry.hpp>
#include <dockyard/mesh.hpp>
#include <dockyard/mesh_loader.hpp>
#include <dockyard/pipeline_builder.hpp>
#include <dockyard/scene.hpp>

#include <glm/glm.hpp>

namespace dy {

struct GpuPushConstants {
  const DeviceAddress vertex_buffer_ptr;               // 8 bytes
  const DeviceAddress position_only_vertex_buffer_ptr; // 8 bytes
  const DeviceAddress transform_buffer_ptr;            // 8 bytes
  const DeviceAddress remap_buffer_ptr;                // 8 bytes
  const DeviceAddress frame_ubo;                       // 8 bytes
  const DeviceAddress material_ptr;                    // 8 bytes
};

struct CompositePushConstants {
  const TextureHandle forward_texture_index;
  const SamplerHandle sampler;
};

enum class RenderPassType { DepthPrepass, Forward, Shadow };

struct PendingDraw {
  Mesh mesh;
  u32 pipeline_id;
  u32 material_id;
  glm::mat4 transform;

  [[nodiscard]] constexpr auto get_key(RenderPassType pass) const -> u64;
};

struct RenderPass {
  RenderPassType type;
  VmaAllocator allocator;
  std::unique_ptr<Buffer> indirect_buffer{nullptr};
  std::unique_ptr<Buffer> index_remapping_buffer{nullptr};
  std::unique_ptr<Buffer> count_buffer{nullptr};

  struct Batch {
    u32 pipeline_id;
    u32 max_command_count;
    u32 first_command_index;
    u32 count_buffer_offset;
  };
  std::vector<Batch> batches{};

  void ensure_capacity(usize command_count, usize instance_count,
                       usize batch_count);
  void bake(const std::vector<PendingDraw> &scene_draws);
};

struct InstanceData {
  glm::mat4 transform;
  u32 material_id;
  u32 pad[3];
};

struct FrameUBO {
  glm::mat4 view;
  glm::mat4 projection;
  glm::mat4 view_projection;
  glm::mat4 inverse_projection;
  glm::mat4 inverse_view;
  glm::mat4 inverse_view_projection;
  glm::vec4 camera_position;
};

auto create_main_pipeline_layout(VkDevice device,
                                 VkDescriptorSetLayout bindless_layout)
    -> VkPipelineLayout;
auto create_composite_pipeline(VkDevice device,
                               VkDescriptorSetLayout bindless_layout,
                               VkPipelineLayout &out_layout,
                               VkPipeline &out_pipeline) -> void;

struct SceneRenderer {
  VulkanContext &ctx;
  SwapchainResources &swapchain;

  RenderPass depth_prepass;
  RenderPass forward_pass;

  TexturePool textures;
  SamplerPool samplers;
  ComparisonSamplerPool comparison_samplers;
  BindlessSet bindless;
  std::unique_ptr<GeometryPool> geometry_pool;

  std::deque<MeshAsset> mesh_registry;

  TextureHandle dummy_texture_handle{};
  SamplerHandle dummy_sampler_handle{};
  TextureHandle forward_target_handle{};
  auto update_output_texture(TextureHandle h) { forward_target_handle = h; }

  VkSampler dummy_sampler_vk = VK_NULL_HANDLE;
  VkSampler comparison_sampler_vk = VK_NULL_HANDLE;

  std::vector<InstanceData> global_instance_data{};
  std::vector<PendingDraw> submission_queue{};
  std::unique_ptr<Buffer> global_instance_buffer{nullptr};
  std::vector<std::unique_ptr<Buffer>> frame_ubo_buffers{};

  VkPipelineLayout pipeline_layout{VK_NULL_HANDLE};
  std::unique_ptr<PipelineRegistry> pipeline_registry{nullptr};

  VkPipelineLayout composite_pipeline_layout{VK_NULL_HANDLE};
  VkPipeline composite_pipeline{VK_NULL_HANDLE};

  // -----------------------------------------------------------------------

  explicit SceneRenderer(VulkanContext &c, SwapchainResources &sc);

  auto initialise_bindless(TextureHandle white) -> void;
  auto upload_texture(std::span<const u32> data, std::string_view name, u32 w,
                      u32 h, VkFormat fmt, bool gen_mips, bool storage = true)
      -> TextureHandle;

  auto resize() -> void;
  auto destroy() -> void;

  void submit(MeshHandle handle, const Components::Transform &t, u32 p_id = 0u,
              u32 m_id = 0u);
  void submit(const Mesh &mesh, const Components::Transform &t, u32 p_id,
              u32 m_id);

  void ensure_global_capacity(usize instance_count);
  void prepare(u64 frame_index, const glm::mat4 &view,
               const glm::mat4 &projection);
  void render_pass(VkCommandBuffer cmd, u64 frame_index, GeometryPool &pool,
                   RenderPass &pass,
                   VkPipeline override_pipeline = VK_NULL_HANDLE);
  void composite_pass(VkCommandBuffer cmd);

  auto register_gltf(MeshAsset &&asset) -> MeshHandle;
  auto get_mesh(MeshHandle handle) -> MeshAsset *;
};

} // namespace dy
#pragma once

#include "dockyard/bindless_handle.hpp"
#include <bitset>
#include <dockyard/app.hpp>
#include <dockyard/buffer.hpp>
#include <dockyard/compiler.hpp>
#include <dockyard/context.hpp>
#include <dockyard/device_geometry.hpp>
#include <dockyard/freelist_pool.hpp>
#include <dockyard/mesh.hpp>
#include <dockyard/mesh_loader.hpp>
#include <dockyard/pipeline_builder.hpp>
#include <dockyard/scene.hpp>
#include <dockyard/texture.hpp>
#include <dockyard/texture_upload_pool.hpp>
#include <dockyard/thread_safe_memory_cache.hpp>

#include <dockyard/renderer_settings_registry.hpp>

#include <BS_thread_pool.hpp>
#include <deque>
#include <glm/glm.hpp>
#include <span>
#include <type_traits>

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
  const DeviceAddress skinned_vertex_buffer_ptr;
  const DeviceAddress skinned_position_buffer_ptr;
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

  u32 total_instance_count;
  u32 padding[3];
};

struct ResetPushConstants {
    DeviceAddress base;
    u32 element_count;
    u32 stride_in_u32;
    u32 offset_in_u32;
    u32 reset_value;
};

struct OcclusionCullingPushConstants {
  DeviceAddress instance_buffer;
  DeviceAddress frame_data;

  DeviceAddress forward_original_remap_buffer;
  DeviceAddress forward_instance_to_command_buffer;
  DeviceAddress forward_indirect_commands;
  DeviceAddress forward_culled_remap;

  u32 total_instance_count;
  u32 hiz_sampler_idx;
  f32 hiz_width;
  f32 hiz_height;
  // uint4[4], not u32[16]: matches the shader's 16-byte-per-element stride
  // under any block layout. Flat index i lives at [i / 4][i % 4]; flat [15] is
  // the mip count. See the shader struct for the rationale.
  glm::uvec4 hiz_mip_indices[4]{};
};

struct HizPushConstants {
  u32 src_texture_idx;
  u32 dst_texture_idx;
  VkExtent2D src_dimension;
};

struct SkinningPushConstants {
  DeviceAddress src_vertices;
  DeviceAddress src_positions;
  DeviceAddress skin_attrs;
  DeviceAddress joint_palette;
  DeviceAddress dst_vertices;
  DeviceAddress dst_positions;
  u32 src_vertex_offset;
  u32 skin_vertex_offset;
  u32 dst_vertex_offset;  // also stored as CompressedInstanceData::padding0
  u32 vertex_count;
  u32 joint_palette_offset; // in glm::mat4 units
  u32 _pad{};
};
static_assert(sizeof(SkinningPushConstants) == 72);

// One dispatch job per skinned instance per frame.
struct SkinJob {
  u32 src_vertex_offset;    // into vertex_buffer (Vertex units)
  u32 skin_vertex_offset;   // into skin_vertex_buffer (SkinVertex units)
  u32 dst_vertex_offset;    // into per-frame scratch (Vertex units)
  u32 vertex_count;
  u32 joint_palette_offset; // into joint_palette_buffers (mat4 units)
};

enum class RenderPassType : u8 { DepthPrepass, Forward, Shadow };

struct PendingDraw {
  const MeshLodGroup *lod_group;
  u32 pipeline_id;
  u32 material_id;
  glm::mat4 transform;
  AABB aabb;

  u32 instance_id;
};

struct FrameSubmission {
  struct Entry {
    u64 sort_key;
    u32 mesh_prim_flat_index;
    u32 material_id;
    u32 pipeline_id;
    glm::mat4 transform;
    AABB aabb;
    u32 skinned_base = ~0u; // dst_vertex_offset in scratch; ~0u = not skinned
  };

  std::vector<Entry> entries;
  std::vector<u32> sort_order;

  auto reset(usize hint) -> void {
    entries.clear();
    sort_order.clear();
    entries.reserve(hint);
    sort_order.reserve(hint);
  }
};

struct RenderPass {
  RenderPassType type;
  VmaAllocator allocator;
  SceneRenderer &renderer;

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
  auto bake(std::span<const u32> sorted_order,
            std::span<const FrameSubmission::Entry> entries,
            usize total_global_instances) -> void;
};

struct InstanceMetadata {
  u32 material_id : 16;
  u32 lod_count : 3;
  u32 padding : 13;
};
union PackedDataSlot {
  InstanceMetadata metadata;
  u16 half_floats[2]; // [0] = radius, [1] = padding
  float gpu_float;
};
struct alignas(16) CompressedInstanceData {
  glm::mat3x4 transform;
  float material_and_lod;
  float bounding_radius;
  float padding0;
  float padding1;
  CompressedInstanceData() = default;
  CompressedInstanceData(const glm::mat4 &, u16, f32, u8);
};
static_assert(sizeof(CompressedInstanceData) == 64,
              "CompressedInstanceData must be exactly 64 bytes!");

struct alignas(16) CascadeData {
  glm::mat4 view_proj;
  float split_depth; // view-space z of cascade far (negative, RH)
  float _pad[3];
};

struct FlatPrimitive {
  u32 first_index;
  u32 index_count;
  i32 vertex_offset;
  u32 default_material_id;
  const MeshLodGroup *lod_group;
};

static constexpr auto shadow_map_cascade_count = 6;
static constexpr u32 shadow_map_cascade_resolution = 2048;

struct GPUPointLight {
  glm::vec3 position;
  float radius; // falloff distance
  glm::vec3 color;
  float intensity;
};

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
  f32 camera_near;
  f32 camera_far;
  f32 shadow_near;
  f32 shadow_far;
  u32 shadow_array_index;
  u32 shadow_sampler_index;
  u32 ibl_irradiance_index;  // binding 4 (sampled_cubemaps)
  u32 ibl_prefiltered_index; // binding 4 (sampled_cubemaps)
  u32 ibl_brdf_lut_index;    // binding 0 (sampled_images)
  u32 ibl_sampler_index;     // binding 1 (samplers) — linear + mip
  u32 ibl_prefiltered_mips;  // needed for roughness LOD selection
  u32 pad_pre_lights{};      // align point_lights array to 16-byte boundary

  static constexpr u32 max_point_lights = 256;
  std::array<GPUPointLight, max_point_lights> point_lights{};
  u32 point_light_count;
  u32 pad0{};
  u32 pad1{};
  u32 pad2{};
};
static_assert(sizeof(FrameUBO) % 16 == 0);

static_assert(sizeof(CascadeData) == 80);
static_assert(sizeof(GPUPointLight) == 32);
static_assert(sizeof(FrameUBO) % 16 == 0);
static_assert(offsetof(FrameUBO, point_lights) == 1040);

struct CsmResources {
  VkImage image = VK_NULL_HANDLE;
  VmaAllocation allocation = VK_NULL_HANDLE;
  VkImageView array_view = VK_NULL_HANDLE;
  std::array<VkImageView, shadow_map_cascade_count> layer_views{};

  TextureHandle bindless_handle;
  std::array<TextureHandle, shadow_map_cascade_count> layer_handles{};

  void destroy(VkDevice device, VmaAllocator allocator);
};

struct ProfilingContext;

struct SceneRenderer {
  VulkanContext &ctx;
  SwapchainResources &swapchain;
  std::unique_ptr<ProfilingContext,
                  decltype(+[](ProfilingContext *) -> void {})>
      tracy_vk_ctx;

  BS::priority_thread_pool thread_pool;

  RenderPass depth_prepass;
  RenderPass forward_pass;

  TexturePool textures;
  SamplerPool samplers;
  ComparisonSamplerPool comparison_samplers;
  SubImagePool subimages;
  BindlessSet bindless;
  std::unique_ptr<GeometryPool> geometry_pool;
  using MaterialOverridePool = FreeListPool;
  MaterialOverridePool override_pool;

  using MeshAssetPool = Pool<MeshAssetTag, MeshAsset>;
  MeshAssetPool mesh_registry;

  std::unique_ptr<pool::TextureUploadPool> texture_upload_pool{nullptr};

  TextureHandle dummy_texture_handle;
  SamplerHandle dummy_sampler_handle;
  SamplerHandle hiz_sampler_handle;
  SamplerHandle cube_sampler_handle;
  TextureHandle white_texture;
  TextureHandle normal_texture;
  TextureHandle metallic_roughness_texture;
  TextureHandle ambient_occlusion_texture;
  TextureHandle black_texture;

  TextureHandle forward_target_handle;
  auto update_output_texture(TextureHandle h) { forward_target_handle = h; }

  VkSampler dummy_sampler_vk = VK_NULL_HANDLE;
  VkSampler comparison_sampler_vk = VK_NULL_HANDLE;

  std::vector<CompressedInstanceData> global_instance_data{};
  std::vector<PendingDraw> submission_queue{};

  FrameSubmission frame_submission;
  std::vector<FlatPrimitive> flat_prim_table;

  FrameArray<std::unique_ptr<Buffer>> global_instance_buffer{nullptr};
  FrameArray<std::unique_ptr<Buffer>> frame_ubo_buffers{};

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
  std::optional<VFSPath> pending_hdr_map;

  std::array<CascadeData, shadow_map_cascade_count> cascade_cpu_data{};
  glm::vec4 sun_direction =
      glm::normalize(glm::vec4{0, 0, 0, 0} - glm::vec4{3, -7, 5, 0});

  f32 skybox_lod = 0.7F;

  RendererSettingsRegistry settings_registry;

  PipelineHandle shadow_pipeline;
  PipelineHandle composite_pipeline;
  PipelineHandle skybox_pipeline;
  PipelineHandle depth_only_culling_pipeline;
  PipelineHandle depth_to_r32_pipeline;
  PipelineHandle forward_occlusion_pipeline;
  PipelineHandle hiz_downsample_pipeline;
  PipelineHandle skinning_pipeline;
  PipelineHandle buffer_reset_pipeline;

  // Per-frame buffers for GPU skinning.
  // joint_palette_buffers holds all joint matrices for all skinned entities,
  // concatenated (glm::mat4 per joint, indexed by SkinJob::joint_palette_offset).
  FrameArray<std::unique_ptr<Buffer>> joint_palette_buffers;
  // Scratch regions that the skinning compute writes into; vertex shaders read
  // from these via device address stored in CompressedInstanceData::padding0/1.
  FrameArray<std::unique_ptr<Buffer>> skinned_vertex_scratch;
  FrameArray<std::unique_ptr<Buffer>> skinned_position_scratch;
  usize skinned_scratch_capacity  = 0; // in Vertex units
  usize joint_palette_capacity    = 0; // in mat4 units

  // Skin jobs queued this frame; cleared by skinning_pass().
  std::vector<SkinJob> pending_skin_jobs;
  // CPU-side joint palette accumulator; uploaded to GPU in prepare().
  std::vector<glm::mat4> pending_palette_data;
  // Per-frame accumulators; reset in prepare().
  u32 frame_skin_dst_vertex   = 0; // next free offset in scratch (Vertex units)
  u32 frame_palette_mat_count = 0; // next free slot in joint_palette_buffers (mat4 units)

  Cache<StringMap<TextureHandle>> texture_cache{};

  // -----------------------------------------------------------------------

  explicit SceneRenderer(VulkanContext &c, SwapchainResources &sc);

  auto initialise_bindless() -> void;
  auto initialise_settings() -> void;
  void init_csm();
  auto upload_texture(std::span<const std::byte> data, std::string_view name,
                      u32 w, u32 h, VkFormat fmt, bool gen_mips,
                      bool storage = true) -> TextureHandle;

  auto resize() -> void;
  auto destroy() -> void;

  void ensure_global_capacity(usize instance_count);

  struct PrepareResult {
    enum class Status : u8 {
      Success,
      SuccessMaterialPoolGrew,
      SuccessNoSubmissions,
      DeviceWaitRequired,
    };

    Status status = Status::Success;
    u32 material_pool_delta = 0;

    [[nodiscard]] auto failed() const {
      return status == Status::DeviceWaitRequired;
    }
  };
  struct FrameRenderInfo {
    u64 frame_index;
    glm::mat4 view;
    glm::mat4 projection;
    glm::vec2 camera_near_far;
    glm::vec2 shadow_near_far;
    std::span<const GPUPointLight> point_lights;
  };
  auto prepare(const FrameRenderInfo &) -> PrepareResult;

  void submit(MeshAssetHandle handle, const glm::mat4 &, u32 pipeline_id = 0U,
              u32 material_id = 0U);
  void submit(MeshAssetHandle handle, const glm::mat4 &,
              std::span<const glm::mat4> joint_palette,
              u32 pipeline_id = 0U, u32 material_id = 0U);

  void update_csm(const glm::mat4 &view, const glm::mat4 &proj,
                  float camera_near, float camera_far);
  void render_shadow_cascade(VkCommandBuffer cmd, u32 cascade_idx);
  void render_pass(VkCommandBuffer, RenderPass &,
                   VkPipeline override_pipeline = VK_NULL_HANDLE);
  void composite_pass(VkCommandBuffer);
  void skybox_pass(VkCommandBuffer);
  void depth_frustum_culling_pass(VkCommandBuffer);
  // Dispatches one compute job per pending SkinJob; inserts a compute→vertex
  // barrier after all jobs. Clears pending_skin_jobs on return.
  void skinning_pass(VkCommandBuffer);
  // Ensures per-frame scratch buffers can hold at least vertex_count vertices.
  void ensure_skinned_scratch(usize vertex_count);
  // Ensures per-frame joint palette buffers can hold at least mat_count mat4s.
  void ensure_joint_palette_capacity(usize mat_count);
  auto blit_depth_to_pre_hiz_pass(VkCommandBuffer, TextureHandle depth_resolved,
                                  TextureHandle depth_pre_hiz) -> void;
  void build_hierarchical_depth_pyramid_pass(VkCommandBuffer,
                                             TextureHandle input_depth_image,
                                             TextureHandle output_pyramid);
  void forward_occlusion_culling_pass(VkCommandBuffer,
                                      TextureHandle hiz_target);
  void reset_indirect_counts(VkCommandBuffer , RenderPass &);
  template <typename T>
  void reset_field(VkCommandBuffer cmd, Buffer &buf,
                                  u32 count, u32 field_offset, u32 value = 0U) {
    static_assert(std::is_standard_layout_v<T>);
    static_assert(sizeof(T) % 4 == 0, "stride must be u32-aligned");

    const auto &entry = pipeline_registry->get_entry(buffer_reset_pipeline);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, entry.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, entry.layout,
                            0U, 1U, &bindless.set, 0U, nullptr);

    const ResetPushConstants pc{
        .base = buf.get_device_address(),
        .element_count = count,
        .stride_in_u32 = sizeof(T) / 4,
        .offset_in_u32 = field_offset / 4,
        .reset_value = value,
    };
    vkCmdPushConstants(cmd, entry.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0U,
                       sizeof(pc), &pc);
    vkCmdDispatch(cmd, (count + 63U) / 64U, 1U, 1U);
  }

  auto register_gltf(MeshAsset &&asset) -> MeshAssetHandle;
  auto register_external_view(VkImageView view, VkImageViewType type)
      -> TextureHandle;
  auto get_mesh(MeshAssetHandle handle) -> MeshAsset *;
  auto get_mesh(MeshAssetHandle handle) const -> const MeshAsset *;
  [[nodiscard]] auto get_material_view(MeshAssetHandle handle) const
      -> ConstMaterialView;
  [[nodiscard]] auto get_material_view_mut(MeshAssetHandle handle)
      -> MutableMaterialView;
  [[nodiscard]] auto get_material_view(const MeshAsset &) const
      -> ConstMaterialView;
  [[nodiscard]] auto get_material_view_mut(const MeshAsset &) const
      -> MutableMaterialView;
  auto remove_override(Entity) -> void;

  auto set_hdr_map(VFSPath) -> void;
  auto process_pending_hdr_map() -> void;
  static auto create_ibl_probe_from_hdr(SceneRenderer &renderer,
                                        const VFSPath &path)
      -> std::expected<IblProbe, std::string>;

  template <typename Handle>
  auto resolve(Handle handle) const -> decltype(auto) {
    if constexpr (std::is_same_v<Handle, TextureHandle>) {
      return textures.get(handle)->texture;
    } else if constexpr (std::is_same_v<Handle, SamplerHandle>) {
      return samplers.get(handle)->sampler;
    } else if constexpr (std::is_same_v<Handle, ComparisonSamplerHandle>) {
      return comparison_samplers.get(handle)->sampler;
    } else if constexpr (std::is_same_v<Handle, MeshAssetHandle>) {
      return mesh_registry.get(handle);
    } else {
      static_assert(false, "Unsupported handle type");
    }
  }

  template <typename Handle> auto resolve_mut(Handle handle) -> decltype(auto) {
    if constexpr (std::is_same_v<Handle, TextureHandle>) {
      return textures.get(handle)->texture;
    } else if constexpr (std::is_same_v<Handle, SamplerHandle>) {
      return samplers.get(handle)->sampler;
    } else if constexpr (std::is_same_v<Handle, ComparisonSamplerHandle>) {
      return comparison_samplers.get(handle)->sampler;
    } else if constexpr (std::is_same_v<Handle, MeshAssetHandle>) {
      return mesh_registry.get(handle);
    } else {
      static_assert(false, "Unsupported handle type");
    }
  }
};

} // namespace dy

#pragma once

#include <dockyard/bindless_descriptor.hpp>
#include <dockyard/types.hpp>

#include <array>
#include <glm/glm.hpp>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>
#include <vk_mem_alloc.h>

namespace dy {

class Buffer;

enum class DeviceAddress : u64 { Invalid = 0 };

struct Vertex {
  float position[3];
  u32 uvs;
  u32 uvs1;
  u32 normal;
  u32 tangent;
  u32 bitangent;
};
static_assert(std::is_trivially_copyable_v<Vertex>);
static_assert(sizeof(Vertex) == 32); // 12 (pos) + 5*4 (rest)

struct PositionOnlyVertex {
  float position[3];
};
static_assert(std::is_trivially_copyable_v<PositionOnlyVertex>);
static_assert(sizeof(PositionOnlyVertex) == 12);

// Per-vertex skinning attributes, stored sparsely in GeometryPool's
// skin_vertex_buffer parallel to the main vertex stream. Only skinned
// primitives allocate skin vertices; static geometry never touches this buffer.
//
// Joints are packed as two u16 pairs (up to 65536 joints per skin); weights as
// unorm8x4. The skinning compute pass reads this by index alongside Vertex.
struct SkinVertex {
  u32 joints_0_1; // joint[0] | (joint[1] << 16)
  u32 joints_2_3; // joint[2] | (joint[3] << 16)
  u32 weights;    // unorm8x4, normalized to sum to 1
};
static_assert(std::is_trivially_copyable_v<SkinVertex>);
static_assert(sizeof(SkinVertex) == 12);

static constexpr u32 max_joints_per_skin = std::numeric_limits<u16>::max();

// Pack four joint indices and their (un-normalized) influence weights into the
// GPU SkinVertex layout. Weights are renormalized so they sum to 1; an all-zero
// weight vector degenerates to full influence from joint[0].
[[nodiscard]] auto pack_skin_vertex(const std::array<u16, 4> &joints,
                                    const glm::vec4 &weights) -> SkinVertex;

enum class MaterialFlags : u32 {
  None = 0,
  depth_prepass = 1 << 0,    // Render in depth prepass (opaque only)
  alpha_mask = 1 << 1,       // AlphaMode::Mask — discard on cutoff
  has_transmission = 1 << 2, // Shader: check before sampling transmission
  has_anisotropy = 1 << 3,   // Shader: check before using anisotropy
  two_sided = 1 << 4,        // Backface rendering enabled
  combined_orm = 1 << 5, // Shader: occlusion packed in ORM (XYZ=ORM, W=unused)
  no_occlusion =
      1 << 6, // Shader: skip occlusion sampling, use occlusion_strength only
  alpha_blend = 1 << 7,       // AlphaMode::Blend — sorted translucency, no depth write
  spec_gloss_workflow = 1 << 8, // KHR_materials_pbrSpecularGlossiness
};
MAKE_BITFIELD(MaterialFlags)

enum class AlphaMode : u32 { Opaque = 0, Mask = 1, Blend = 2 };
enum class CullMode : u32 {
  Back = 0,  // Cull backfaces (default)
  Front = 1, // Cull frontfaces
  None = 2,  // No culling (two-sided)
};

struct GPUMaterial {
  alignas(16) float albedo_factor[4]{1.0f, 1.0f, 1.0f, 1.0f};
  alignas(16) float emissive_factor[4]{0.0f, 0.0f, 0.0f, 0.0f};

  // PBR factors + scales
  float metallic_factor;
  float roughness_factor;
  float normal_scale;
  float occlusion_strength;

  // Alpha & mode
  u32 alpha_mode;
  float alpha_cutoff;

  // Texture indices
  u32 albedo_index;
  u32 normal_index;
  u32 metallic_roughness_index;
  u32 emissive_index;
  u32 occlusion_index;

  // Flags for shader branching
  MaterialFlags flags;

  // Transmission (glass refraction) — KHR_materials_transmission
  float transmission_factor; // [0,1]: 0 = opaque, 1 = fully transmissive

  // Anisotropy (brushed metals, etc) — KHR_materials_anisotropy
  float anisotropy_factor;   // [0,1]: strength of anisotropic reflection
  float anisotropy_rotation; // [0,1]: rotation angle (normalized to [0, 2π])

  u32 cull_mode;

  // UV transformation (cheap variation without extra textures)
  float uv_scale_x;
  float uv_scale_y;
  float uv_offset_x;
  float uv_offset_y;

  // KHR_materials_pbrSpecularGlossiness: specular F0 color and SG texture index.
  // metallic_factor is repurposed as glossiness_factor in this workflow.
  float specular_factor[3];
  u32 specular_glossiness_index;

  // KHR_materials_specular: per-material F0 tint for dielectrics (MR workflow only).
  float specular_color_factor[3]{1.0F, 1.0F, 1.0F};
  u32 specular_color_index;

  // Per-texture UV channel selector. Bit N = 1 means that texture slot uses
  // UV1 (TEXCOORD_1) instead of UV0. Bit positions: see uv_ch_* constants in
  // push_constant.slang.
  u32 uv_channel_mask;
  u32 _pad_uv[3];
};

static_assert(std::is_trivially_copyable_v<GPUMaterial>);
static_assert(sizeof(GPUMaterial) % 16 == 0,
              "GPUMaterial must be 16-byte aligned");

// CPU-side mirror of GPUMaterial; used for editor workflows and per-instance
// variants.
struct MaterialAsset {
  std::string name;

  // PBR base
  float albedo_factor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  float emissive_factor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  float metallic_factor = 1.0f;
  float roughness_factor = 1.0f;
  float normal_scale = 1.0f;
  float occlusion_strength = 1.0f;

  // Alpha control
  AlphaMode alpha_mode = AlphaMode::Opaque;
  float alpha_cutoff = 0.5f;

  // Textures
  TextureHandle albedo_texture;
  TextureHandle normal_texture;
  TextureHandle metallic_roughness_texture;
  TextureHandle emissive_texture;
  TextureHandle ambient_occlusion_texture;

  // glTF 2.0 extensions
  float transmission_factor = 0.0f; // KHR_materials_transmission
  float anisotropy_factor = 0.0f;   // KHR_materials_anisotropy
  float anisotropy_rotation = 0.0f;
  float specular_factor[3] = {1.0f, 1.0f, 1.0f}; // KHR_materials_pbrSpecularGlossiness
  TextureHandle specular_glossiness_texture;
  float specular_color_factor[3] = {1.0F, 1.0F, 1.0F}; // KHR_materials_specular
  TextureHandle specular_color_texture;

  // Rendering control
  bool double_sided = false;
  CullMode cull_mode = CullMode::Back;

  // UV manipulation
  float uv_scale_x = 1.0f;
  float uv_scale_y = 1.0f;
  float uv_offset_x = 0.0f;
  float uv_offset_y = 0.0f;

  // Per-texture UV channel selector; mirrors GPUMaterial::uv_channel_mask.
  u32 uv_channel_mask = 0;
};

struct AllocatedOffset {
  usize vertex_offset;
  usize shadow_vertex_offset;
  usize index_offset;
};

struct MaterialOffset {
  u32 start_index;
};

namespace detail {

template <typename T> struct BasicMaterialView {
  std::span<T> materials;
  u32 base_slot = 0;

  [[nodiscard]] auto first() const -> const T & {
    assert(!materials.empty());
    return materials[0];
  }
  [[nodiscard]] auto first_mut() -> T & {
    assert(!materials.empty());
    return materials[0];
  }

  [[nodiscard]] auto empty() const -> bool { return materials.empty(); }
  [[nodiscard]] auto size() const -> u32 {
    return static_cast<u32>(materials.size());
  }
  [[nodiscard]] auto operator[](u32 i) const -> T & { return materials[i]; }
  [[nodiscard]] auto slot(u32 i) const -> u32 { return base_slot + i; }
};
} // namespace detail

using MutableMaterialView = detail::BasicMaterialView<GPUMaterial>;
using ConstMaterialView = detail::BasicMaterialView<const GPUMaterial>;

struct GeometryPool;
struct GeometryTransaction {
  GeometryPool &pool;
  usize start_v, start_sv, start_i;
  bool committed = false;

  ~GeometryTransaction();
  auto allocate(std::span<const Vertex>, std::span<const u32>)
      -> AllocatedOffset;
  void commit();
};

struct GeometryPool {
  VmaAllocator allocator{};
  std::unique_ptr<Buffer> vertex_buffer;
  std::unique_ptr<Buffer> position_only_vertex_buffer;
  std::unique_ptr<Buffer> index_buffer;
  std::unique_ptr<Buffer> material_buffer;
  // Lazily created on the first skinned upload; null for scenes with no
  // skinned geometry. Indexed in SkinVertex units, parallel to vertex_buffer.
  std::unique_ptr<Buffer> skin_vertex_buffer;

  usize vertex_offset = 0;
  usize shadow_vertex_offset = 0;
  usize index_offset = 0;
  usize material_offset = 0;
  usize skin_vertex_offset = 0; // in SkinVertex units

  static auto create(VmaAllocator allocator, usize v_size,
                     usize position_only_size, usize i_size, usize m_size)
      -> std::unique_ptr<GeometryPool>;
  auto allocate(std::span<const Vertex> vertices, std::span<const u32> indices)
      -> AllocatedOffset;
  auto allocate_without_flush(std::span<const Vertex> vertices,
                              std::span<const u32> indices) -> AllocatedOffset;
  auto allocate_materials(std::span<const GPUMaterial>) -> MaterialOffset;
  auto allocate_materials(std::span<const MaterialAsset>) -> MaterialOffset;

  void reserve(usize additional_vertices, usize additional_indices);
  void reserve_materials(usize additional_mats);

  // Ensure skin_vertex_buffer exists and can hold skin_vertex_offset +
  // additional_skin_vertices entries, creating or growing it as needed.
  void ensure_skin_capacity(usize additional_skin_vertices);
  // Mapped write pointer to the skin buffer at the given SkinVertex index.
  // Requires ensure_skin_capacity to have been called for the range.
  [[nodiscard]] auto skin_mapped_pointer(usize vertex_index) -> SkinVertex *;
  auto flush_skin_range(usize first_vertex, usize vertex_count) -> void;

  [[nodiscard]] auto get_material(u32 slot) -> GPUMaterial &;
  auto update_material(u32 slot, const GPUMaterial &mat) -> void;
  [[nodiscard]] auto get_materials(u32 base_slot, u32 count) const
      -> std::span<const GPUMaterial>;
  [[nodiscard]] auto get_materials_mut(u32 base_slot, u32 count)
      -> std::span<GPUMaterial>;
  auto flush_material(u32 slot) -> void;
  auto flush_materials(u32 base_slot, u32 count) -> void;

  struct FlushRange {
    usize vertex_offset;
    usize vertex_size;
    usize shadow_vertex_offset;
    usize shadow_vertex_size;
    usize index_offset;
    usize index_size;
  };
  auto flush_range(const FlushRange &range) -> void {
    flush_range(range.vertex_offset, range.vertex_size,
                range.shadow_vertex_offset, range.shadow_vertex_size,
                range.index_offset, range.index_size);
  }
  auto begin_transaction() -> GeometryTransaction {
    return {
        .pool = *this,
        .start_v = vertex_offset,
        .start_sv = shadow_vertex_offset,
        .start_i = index_offset,
    };
  }

private:
  auto flush_range(usize v_off, usize v_size, usize sv_off, usize sv_size,
                   usize i_off, usize i_size) -> void;
};
} // namespace dy

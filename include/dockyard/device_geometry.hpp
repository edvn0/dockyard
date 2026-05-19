#pragma once

#include <dockyard/bindless_descriptor.hpp>
#include <dockyard/types.hpp>

#include <memory>
#include <type_traits>
#include <vk_mem_alloc.h>

namespace dy {

class Buffer;

enum class DeviceAddress : u64 { Invalid = 0 };

struct Vertex {
  float position[3];
  u32 uvs;
  u32 normal;
  u32 tangent;
  u32 bitangent;
};
static_assert(std::is_trivially_copyable_v<Vertex>);
static_assert(sizeof(Vertex) == 28); // 12 (pos) + 4*4 (rest)

struct PositionOnlyVertex {
  float position[3];
};
static_assert(std::is_trivially_copyable_v<PositionOnlyVertex>);
static_assert(sizeof(PositionOnlyVertex) == 12);

struct GPUMaterial {
  alignas(16) float albedo_factor[4];
  alignas(16) float emissive_factor[4];
  float metallic_factor;

  float roughness_factor;
  float normal_scale;
  float occlusion_strength;
  u32 alpha_mode;

  float alpha_cutoff;
  u32 albedo_index;
  u32 normal_index;
  u32 metallic_roughness_index;

  u32 emissive_index;
  u32 occlusion_index;
  u32 pad0;
};
static_assert(std::is_trivially_copyable_v<GPUMaterial>);
static_assert(sizeof(GPUMaterial) % 16 == 0);

enum class AlphaMode : u32 { Opaque = 0, Mask = 1, Blend = 2 };

struct MaterialAsset {
  std::string name;

  float albedo_factor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  float emissive_factor[4] = {0.0f, 0.0f, 0.0f, 0.0F};
  float metallic_factor = 1.0f;
  float roughness_factor = 1.0f;

  float normal_scale = 1.0f;
  float occlusion_strength = 1.0f;

  AlphaMode alpha_mode = AlphaMode::Opaque;
  float alpha_cutoff = 0.5f;

  TextureHandle albedo_texture;
  TextureHandle normal_texture;
  TextureHandle metallic_roughness_texture;
  TextureHandle emissive_texture;
  TextureHandle occlusion_texture;

  auto resolve_to_gpu() const -> GPUMaterial;
};

struct AllocatedOffset {
  const usize vertex_offset;
  const usize shadow_vertex_offset;
  const usize index_offset;
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

  usize vertex_offset = 0;
  usize shadow_vertex_offset = 0;
  usize index_offset = 0;
  usize material_offset = 0;

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

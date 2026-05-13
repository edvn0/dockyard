#pragma once

#include "dockyard/mesh.hpp"
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
  alignas(16) float emissive_factor[3];
  float metallic_factor;

  float roughness_factor;
  float normal_scale;
  float occlusion_strength;
  uint32_t alpha_mode;

  float alpha_cutoff;
  uint32_t albedo_index;
  uint32_t normal_index;
  uint32_t metallic_roughness_index;

  uint32_t emissive_index;
  uint32_t occlusion_index;
  uint32_t pad0;
  uint32_t pad1;
};
static_assert(std::is_trivially_copyable_v<GPUMaterial>);
static_assert(sizeof(GPUMaterial) % 16 == 0);

enum class AlphaMode : u32 { Opaque = 0, Mask = 1, Blend = 2 };

struct MaterialAsset {
  std::string name;

  float albedo_factor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  float emissive_factor[3] = {0.0f, 0.0f, 0.0f};
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
  auto allocate_materials(std::span<const GPUMaterial>) -> MaterialOffset;
  auto allocate_materials(std::span<const MaterialAsset>) -> MaterialOffset;
};
} // namespace dy

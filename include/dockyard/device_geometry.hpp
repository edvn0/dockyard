#pragma once

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

struct AllocatedOffset {
  const usize vertex_offset;
  const usize shadow_vertex_offset;
  const usize index_offset;
};

struct GeometryPool {
  VmaAllocator allocator{};
  std::unique_ptr<Buffer> vertex_buffer;
  std::unique_ptr<Buffer> position_only_vertex_buffer;
  std::unique_ptr<Buffer> index_buffer;

  usize vertex_offset = 0;
  usize shadow_vertex_offset = 0;
  usize index_offset = 0;

  static auto create(VmaAllocator allocator, usize v_size,
                     usize position_only_size, usize i_size)
      -> std::unique_ptr<GeometryPool>;
  auto allocate(std::span<const Vertex> vertices, std::span<const u32> indices)
      -> AllocatedOffset;
};
} // namespace dy

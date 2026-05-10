#include <dockyard/device_geometry.hpp>

#include <dockyard/buffer.hpp>

namespace dy {

auto dy::GeometryPool::create(VmaAllocator allocator, usize v_size,
                              usize shadow_v_size, usize i_size)
    -> std::unique_ptr<GeometryPool> {
  auto pool = std::make_unique<GeometryPool>();
  pool->allocator = allocator;

  pool->vertex_buffer = Buffer::create(allocator, v_size,
                                       VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

  pool->position_only_vertex_buffer = Buffer::create(
      allocator, shadow_v_size,
      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

  pool->index_buffer = Buffer::create(allocator, i_size,
                                      VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

  return pool;
}

auto dy::GeometryPool::allocate(std::span<const Vertex> vertices,
                                std::span<const u32> indices)
    -> AllocatedOffset {

  const usize v_bytes = vertices.size_bytes();
  const usize i_bytes = indices.size_bytes();
  const usize sv_bytes = vertices.size() * sizeof(PositionOnlyVertex);

  auto *v_dst = static_cast<uint8_t *>(vertex_buffer->get_mapped_pointer());
  auto *sv_dst =
      static_cast<uint8_t *>(position_only_vertex_buffer->get_mapped_pointer());
  auto *i_dst = static_cast<uint8_t *>(index_buffer->get_mapped_pointer());

  std::ranges::copy(vertices,
                    reinterpret_cast<Vertex *>(v_dst + vertex_offset));

  std::ranges::copy(indices, reinterpret_cast<u32 *>(i_dst + index_offset));

  auto position_view =
      vertices |
      std::views::transform([](const Vertex &v) -> PositionOnlyVertex {
        return {.position = {v.position[0], v.position[1], v.position[2]}};
      });
  std::ranges::copy(position_view, reinterpret_cast<PositionOnlyVertex *>(
                                       sv_dst + shadow_vertex_offset));

  vmaFlushAllocation(allocator, vertex_buffer->get_allocation(), vertex_offset,
                     v_bytes);
  vmaFlushAllocation(allocator, index_buffer->get_allocation(), index_offset,
                     i_bytes);
  vmaFlushAllocation(allocator, position_only_vertex_buffer->get_allocation(),
                     shadow_vertex_offset, sv_bytes);

  AllocatedOffset offsets{vertex_offset, shadow_vertex_offset, index_offset};

  vertex_offset += v_bytes;
  shadow_vertex_offset += sv_bytes;
  index_offset += i_bytes;

  return offsets;
}

} // namespace dy

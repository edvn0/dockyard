#include <dockyard/device_geometry.hpp>

#include <dockyard/buffer.hpp>

namespace dy {

auto GeometryPool::create(VmaAllocator allocator, usize v_size,
                          usize shadow_v_size, usize i_size, usize m_size)
    -> std::unique_ptr<GeometryPool> {
  auto pool = std::make_unique<GeometryPool>();
  pool->allocator = allocator;

  const VkBufferUsageFlags common_usage =
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

  pool->vertex_buffer = Buffer::create(
      allocator, v_size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | common_usage);

  pool->position_only_vertex_buffer =
      Buffer::create(allocator, shadow_v_size,
                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | common_usage);

  pool->index_buffer = Buffer::create(
      allocator, i_size, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | common_usage);

  pool->material_buffer = Buffer::create(allocator, m_size, common_usage);

  GPUMaterial default_mat{};
  std::fill_n(default_mat.albedo_factor, 4, 1.0f);
  std::fill_n(default_mat.emissive_factor, 3, 0.0f);
  default_mat.roughness_factor = 0.5f;
  default_mat.metallic_factor = 0.0f;

  default_mat.albedo_index = 0;
  default_mat.normal_index = 1;
  default_mat.metallic_roughness_index = 2;
  default_mat.emissive_index = 3;
  default_mat.occlusion_index = 4;

  pool->allocate_materials(std::span(&default_mat, 1));
  return pool;
}

auto resize_buffer(VmaAllocator allocator, std::unique_ptr<Buffer> &old_buffer,
                   usize required_size) {
  usize new_size = old_buffer->size() * 2;
  while (new_size < required_size)
    new_size *= 2;

  auto new_buffer =
      Buffer::create(allocator, new_size, old_buffer->get_usage_flags());

  std::memcpy(new_buffer->get_mapped_pointer(),
              old_buffer->get_mapped_pointer(), old_buffer->size());

  old_buffer = std::move(new_buffer);
}

auto dy::GeometryPool::allocate(std::span<const Vertex> vertices,
                                std::span<const u32> indices)
    -> AllocatedOffset {

  const usize v_bytes = vertices.size_bytes();
  const usize i_bytes = indices.size_bytes();
  const usize sv_bytes = vertices.size() * sizeof(PositionOnlyVertex);

  // Check for resize
  if (vertex_offset + v_bytes > vertex_buffer->size()) {
    resize_buffer(allocator, vertex_buffer, vertex_offset + v_bytes);
  }
  if (index_offset + i_bytes > index_buffer->size()) {
    resize_buffer(allocator, index_buffer, index_offset + i_bytes);
  }
  if (shadow_vertex_offset + sv_bytes > position_only_vertex_buffer->size()) {
    resize_buffer(allocator, position_only_vertex_buffer,
                  shadow_vertex_offset + sv_bytes);
  }

  auto *v_dst = static_cast<u8 *>(vertex_buffer->get_mapped_pointer());
  auto *sv_dst =
      static_cast<u8 *>(position_only_vertex_buffer->get_mapped_pointer());
  auto *i_dst = static_cast<u8 *>(index_buffer->get_mapped_pointer());

  std::ranges::copy(vertices, std::bit_cast<Vertex *>(v_dst + vertex_offset));
  std::ranges::copy(indices, std::bit_cast<u32 *>(i_dst + index_offset));

  auto position_view =
      vertices |
      std::views::transform([](const Vertex &v) -> PositionOnlyVertex {
        return {.position = {v.position[0], v.position[1], v.position[2]}};
      });
  std::ranges::copy(position_view, std::bit_cast<PositionOnlyVertex *>(
                                       sv_dst + shadow_vertex_offset));

  // Flush updated regions
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

auto GeometryPool::allocate_materials(std::span<const GPUMaterial> mats)
    -> MaterialOffset {

  const usize required_bytes =
      (material_offset + mats.size()) * sizeof(GPUMaterial);
  if (required_bytes > material_buffer->size()) {
    resize_buffer(allocator, material_buffer, required_bytes);
  }

  const u32 start_index = static_cast<u32>(material_offset);
  const usize byte_offset = start_index * sizeof(GPUMaterial);

  material_buffer->upload_with_offset(mats, byte_offset);
  material_offset += mats.size();

  return {start_index};
}

auto GeometryPool::allocate_materials(std::span<const MaterialAsset> mats)
    -> MaterialOffset {
  std::vector<GPUMaterial> gpu_materials;
  for (const auto &asset : mats) {
    GPUMaterial gpu{};

    std::memcpy(gpu.albedo_factor, asset.albedo_factor, sizeof(float) * 4);
    std::memcpy(gpu.emissive_factor, asset.emissive_factor, sizeof(float) * 3);
    gpu.metallic_factor = asset.metallic_factor;
    gpu.roughness_factor = asset.roughness_factor;
    gpu.normal_scale = asset.normal_scale;
    gpu.occlusion_strength = asset.occlusion_strength;
    gpu.alpha_mode = static_cast<u32>(asset.alpha_mode);
    gpu.alpha_cutoff = asset.alpha_cutoff;

    gpu.albedo_index = 0; // tex_system.get_index(asset.albedo_texture);
    gpu.normal_index = 1; // tex_system.get_index(asset.normal_texture);
    gpu.metallic_roughness_index =
        2; // tex_system.get_index(asset.metallic_roughness_texture);
    gpu.emissive_index = 3;  // tex_system.get_index(asset.emissive_texture);
    gpu.occlusion_index = 4; // tex_system.get_index(asset.occlusion_texture);

    gpu_materials.push_back(gpu);
  }
  return allocate_materials(gpu_materials);
}

} // namespace dy

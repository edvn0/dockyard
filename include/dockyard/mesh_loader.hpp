#pragma once

#include <dockyard/app.hpp>
#include <dockyard/device_geometry.hpp>
#include <dockyard/mesh.hpp>
#include <dockyard/vfs_path.hpp>

#include <glm/glm.hpp>

#include <expected>
#include <string>
#include <vector>

namespace dy {

struct SceneRenderer;

struct MeshPrimitiveReference {
  Mesh mesh;
  u32 material_id;
  AABB aabb;
};

struct MeshNodeDescription {
  std::string name;
  glm::mat4 local_transform{1.f};
  i32 parent_index{-1};
  std::vector<MeshPrimitiveReference> primitives;
};

struct MeshAsset {
  std::vector<TextureHandle> texture_handles; // [gltf_image_idx]
  std::vector<u32> material_slots;            // [gltf_material_idx]
  std::vector<std::vector<Mesh>> meshes;

  std::vector<std::vector<AABB>> submesh_aabbs;
  AABB mesh_aabb;

  usize vertex_base_offset{};
  usize shadow_vertex_base_offset{};
  usize index_base_offset{};

  u32 material_base_slot{};
  u32 material_count{};

  std::vector<MeshNodeDescription> nodes;
  std::vector<u32> root_node_indices;
};

namespace mesh {

auto load_from_path_old(const VFSPath &, SceneRenderer &)
    -> std::expected<MeshHandle, std::string>;
auto load_from_memory(SceneRenderer &, std::span<const Vertex>,
                      std::span<const u32>)
    -> std::expected<MeshHandle, std::string>;
auto load_from_path(const VFSPath &, SceneRenderer &)
    -> std::expected<MeshHandle, std::string>;
} // namespace mesh

} // namespace dy
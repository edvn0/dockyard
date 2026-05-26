#pragma once

#include <dockyard/bindless_handle.hpp>
#include <dockyard/types.hpp>
#include <glm/glm.hpp>
#include <limits>

namespace dy {

class AABB {
  static constexpr float k_inf = std::numeric_limits<float>::max();

  glm::vec3 min{k_inf};
  glm::vec3 max{-k_inf};

  AABB(const glm::vec3 &in_min, const glm::vec3 &in_max)
      : min(in_min), max(in_max) {}

public:
  static auto create() -> AABB { return {}; }
  static auto create(const glm::vec3 &in_min, const glm::vec3 &in_max) -> AABB {
    return AABB{in_min, in_max};
  }
  AABB() = default;

  auto update(const glm::vec3 &v) -> void {
    min = glm::min(min, v);
    max = glm::max(max, v);
  }

  [[nodiscard]] auto get_min() const -> const glm::vec3 & { return min; }
  [[nodiscard]] auto get_max() const -> const glm::vec3 & { return max; }

  [[nodiscard]] auto is_valid() const -> bool {
    return min.x <= max.x && min.y <= max.y && min.z <= max.z;
  }

  auto merge(const AABB &other) -> void {
    min = glm::min(min, other.min);
    max = glm::max(max, other.max);
  }

  [[nodiscard]] auto transform(const glm::mat4 &m) const -> AABB {
    AABB result;
    for (int i = 0; i < 8; ++i) {
      const glm::vec3 corner{
          (i & 1) ? max.x : min.x,
          (i & 2) ? max.y : min.y,
          (i & 4) ? max.z : min.z,
      };
      result.update(glm::vec3(m * glm::vec4(corner, 1.0f)));
    }
    return result;
  }
};

struct Mesh {
  u32 index_count;
  u32 first_index;
  i32 vertex_offset;
  auto operator<=>(const Mesh &) const = default;
};

struct SceneRenderer;

struct MeshLod {
  u32 index_count = 0;
  u32 first_index = 0;
};

static constexpr auto k_lod_min_triangles = 300UZ;
static constexpr auto max_lods = 6UZ;
[[nodiscard]] constexpr auto should_generate_lods(const auto &prim) -> bool {
  return (prim.indices.size() / 3) >= k_lod_min_triangles;
}

struct MeshLodGroup {
  i32 vertex_offset = 0;
  u8 lod_count = 1;
  std::array<MeshLod, max_lods> lods{};

  [[nodiscard]] Mesh resolve(u8 idx) const noexcept {
    idx = std::min(idx, static_cast<u8>(lod_count - 1));
    return {
        .index_count = lods[idx].index_count,
        .first_index = lods[idx].first_index,
        .vertex_offset = vertex_offset,
    };
  }
};

struct MeshPrimitiveReference {
  MeshLodGroup lod_group;
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
  std::vector<std::vector<MeshLodGroup>> meshes;

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

} // namespace dy
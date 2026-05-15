#pragma once

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

private:
  AABB() = default;
};

struct Mesh {
  u32 index_count;
  u32 first_index;
  i32 vertex_offset;
  auto operator<=>(const Mesh &) const = default;
};

struct MeshHandle {
  u32 value{~0u};
  [[nodiscard]] auto index() const -> u32 { return value; }
  [[nodiscard]] auto valid() const -> bool { return value != ~0u; }
  auto operator<=>(const MeshHandle &) const = default;
};

} // namespace dy
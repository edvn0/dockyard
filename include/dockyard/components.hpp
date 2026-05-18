#pragma once

#include <dockyard/mesh.hpp>
#include <dockyard/vfs_path.hpp>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtx/quaternion.hpp>

#include <entt/entt.hpp>

namespace dy::Components {

struct MeshRequest {
  VFSPath path;
};

struct Mesh {
  MeshHandle handle;

  explicit(false) operator MeshHandle() const { return handle; }
};

struct Camera {
  float fov_degrees = 70.0f;
  float near_plane = 0.7F;
  float far_plane = 1000.0f;
  float aspect = 16.0f / 9.0f;

  glm::vec3 position{0.0f};
  float yaw = 0.0f;
  float pitch = 0.0f;

  std::optional<glm::vec3> forward_override = std::nullopt;

  [[nodiscard]] auto forward() const -> glm::vec3 {
    if (forward_override.has_value())
      return *forward_override;

    return glm::normalize(glm::vec3{
        glm::cos(pitch) * glm::sin(yaw),
        glm::sin(pitch),
        glm::cos(pitch) * glm::cos(yaw),
    });
  }

  [[nodiscard]] auto right() const -> glm::vec3 {
    return glm::normalize(glm::cross(glm::vec3{0.0f, 1.0f, 0.0f}, forward()));
  }

  [[nodiscard]] auto view() const -> glm::mat4 {
    return glm::lookAtLH(position, position + forward(),
                         glm::vec3{0.0f, 1.0f, 0.0f});
  }

  [[nodiscard]] auto projection() const -> glm::mat4 {
    return glm::perspectiveLH_ZO(glm::radians(fov_degrees), aspect, far_plane,
                                 near_plane);
  }

  auto set_aspect(u32 w, u32 h) -> void {
    aspect = static_cast<float>(w) / static_cast<float>(h);
  }

  static auto facing_toward(glm::vec3 from, glm::vec3 to)
      -> std::pair<float, float> {
    auto dir = glm::normalize(to - from);
    float p = glm::asin(glm::clamp(dir.y, -1.0f, 1.0f));
    float y = glm::atan(dir.x, dir.z);
    return {y, p};
  }
};

struct Tag {
  std::string tag;
  explicit Tag(const std::string_view n) : tag(n) {}
};

struct Transform {
  glm::vec3 position{};
  glm::quat rotation{};
  glm::vec3 scale{1};

  [[nodiscard]] auto matrix() const -> glm::mat4 {
    glm::mat4 res = glm::translate(glm::mat4(1.0f), position);
    res *= glm::mat4_cast(rotation);
    res = glm::scale(res, scale);
    return res;
  }
};

} // namespace dy::Components

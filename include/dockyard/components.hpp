#pragma once

#include <dockyard/device_geometry.hpp>
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
  float fov_degrees = 70.0F;
  float near_plane = 0.7F;
  float far_plane = 1000.0F;
  float aspect = 16.0F / 9.0F;

  glm::vec3 position{0.0F};
  float yaw = 0.0F;
  float pitch = 0.0F;

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
    return glm::normalize(glm::cross(glm::vec3{0.0F, 1.0F, 0.0F}, forward()));
  }

  [[nodiscard]] auto view() const -> glm::mat4 {
    return glm::lookAtLH(position, position + forward(),
                         glm::vec3{0.0F, 1.0F, 0.0F});
  }

  [[nodiscard]] auto projection() const -> glm::mat4 {
    return glm::perspectiveLH_ZO(glm::radians(fov_degrees), aspect, far_plane,
                                 near_plane);
  }

  auto set_aspect(u32 width, u32 height) -> void {
    aspect = static_cast<float>(width) / static_cast<float>(height);
  }

  static auto facing_toward(const auto &from, const auto &vec_to)
      -> std::pair<float, float> {
    auto dir = glm::normalize(vec_to - from);
    float p = glm::asin(glm::clamp(dir.y, -1.0F, 1.0F));
    float y = glm::atan(dir.x, dir.z);
    return {y, p};
  }
};

struct Tag {
  std::string tag;
  explicit Tag(const std::string_view n) : tag(n) {}
};

class Transform {
private:
  struct Accessor {
    glm::vec3 &position;
    glm::quat &rotation;
    glm::vec3 &scale;
  };

  struct ConstAccessor {
    const glm::vec3 &position;
    const glm::quat &rotation;
    const glm::vec3 &scale;
  };

  glm::vec3 position{};
  glm::quat rotation{};
  glm::vec3 scale{1};
  bool is_dirty{true};

public:
  auto set_dirty(bool dirty) -> void { is_dirty = dirty; }
  [[nodiscard]] auto dirty() const -> bool { return is_dirty; }

  [[nodiscard]] auto mut() -> Accessor {
    is_dirty = true;
    return {.position = position, .rotation = rotation, .scale = scale};
  }

  [[nodiscard]] auto get() const -> ConstAccessor {
    return {.position = position, .rotation = rotation, .scale = scale};
  }

  [[nodiscard]] auto matrix() const -> glm::mat4 {
    glm::mat4 res = glm::translate(glm::mat4(1.0F), position);
    res *= glm::mat4_cast(rotation);
    res = glm::scale(res, scale);
    return res;
  }
};

struct LocalToWorld {
  glm::mat4 matrix{1.0F};
};

struct MaterialOverride {
  GPUMaterial material{};
  u32 gpu_slot = ~0U;
  bool dirty = true;
};

struct DebugLine {
  glm::vec3 p1{};
  glm::vec3 p2{};
  glm::vec4 color{1.0F, 1.0F, 1.0F, 1.0F};
};

struct DebugBox {
  glm::vec3 size{1.0F, 1.0F, 1.0F};
  glm::vec4 color{1.0F, 1.0F, 1.0F, 1.0F};
};

struct DebugPlane {
  glm::vec3 v1{1.0F, 0.0F, 0.0F};
  glm::vec3 v2{0.0F, 0.0F, 1.0F};
  int n1 = 10;
  int n2 = 10;
  float s1 = 10.0F;
  float s2 = 10.0F;
  glm::vec4 color{0.4F, 0.4F, 0.4F, 0.5F};
  glm::vec4 outline{0.8F, 0.8F, 0.8F, 1.0F};
};

struct DebugFrustum {
  struct ProjectionConfiguration {
    float fov_degrees{30.0F};
    float aspect{1.77F};
    float near{0.1F};
    float far{30.0F};
  };
  glm::vec3 center{1.0F};
  ProjectionConfiguration projection_config{};
  glm::vec4 color{1.0F, 1.0F, 0.0F, 1.0F};

  [[nodiscard]] auto matrices(const glm::vec3 &) const
      -> std::pair<glm::mat4, glm::mat4>;
};

struct ParentOf {
  entt::entity parent{entt::null};
};

} // namespace dy::Components

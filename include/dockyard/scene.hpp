#pragma once

#include <glm/glm.hpp>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtx/quaternion.hpp>

namespace dy {

namespace Components {

struct Camera {
  float fov_degrees = 70.0f;
  float near_plane = 0.7F;
  float far_plane = 1000.0f;
  float aspect = 16.0f / 9.0f;

  glm::vec3 position{0.0f};
  float yaw = 0.0f;
  float pitch = 0.0f;

  [[nodiscard]] auto forward() const -> glm::vec3 {
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

} // namespace Components

// ---------------------------------------------------------------------------
// Entity / Scene
// ---------------------------------------------------------------------------

class Entity {
  entt::registry &reg;
  entt::entity entity;

public:
  Entity(entt::registry &r, entt::entity_like auto identifier)
      : reg(r), entity(identifier) {}

  explicit Entity(entt::registry &r) : reg(r), entity(reg.create()) {}

  [[nodiscard]] auto handle() const -> entt::entity { return entity; }

  template <typename T, typename... Args>
  auto emplace(Args &&...args) -> decltype(auto) {
    return reg.emplace<T>(entity, std::forward<Args>(args)...);
  }
  template <typename T> auto get() -> decltype(auto) {
    return reg.get<T>(entity);
  }
  template <typename T> auto try_get() -> T * { return reg.try_get<T>(entity); }
};

class Scene {
  entt::registry scene;
  entt::entity primary_camera_entity{entt::null};

public:
  template <typename... Ts> auto view(auto &&...excludes) -> decltype(auto) {
    return scene.view<Ts...>(std::forward<decltype(excludes)>(excludes)...);
  }

  template <typename... Ts> auto group(auto &&...excludes) -> decltype(auto) {
    return scene.group<Ts...>(std::forward<decltype(excludes)>(excludes)...);
  }

  template <typename... Ts> auto intialise_group(auto &&...excludes) {
    std::ignore =
        scene.group<Ts...>(std::forward<decltype(excludes)>(excludes)...);
  }

  auto make(std::string_view name) -> Entity {
    auto e = Entity{scene};
    e.emplace<Components::Tag>(name);
    e.emplace<Components::Transform>();
    return e;
  }

  auto make_camera(std::string_view name, u32 width, u32 height,
                   glm::vec3 position = {0.0f, 0.0f, 0.0f},
                   glm::vec3 look_at = {0.0f, 0.0f, 1.0f}) -> Entity {
    auto e = make(name);
    auto &cam = e.emplace<Components::Camera>();
    cam.position = position;
    cam.set_aspect(width, height);
    auto [y, p] = Components::Camera::facing_toward(position, look_at);
    cam.yaw = y;
    cam.pitch = p;
    primary_camera_entity = e.handle();
    return e;
  }

  auto set_primary_camera(entt::entity e) -> void { primary_camera_entity = e; }
  auto clear_primary_camera() -> void { primary_camera_entity = entt::null; }

  [[nodiscard]] auto primary_camera() -> Components::Camera * {
    if (primary_camera_entity == entt::null)
      return nullptr;
    return scene.try_get<Components::Camera>(primary_camera_entity);
  }
};

} // namespace dy
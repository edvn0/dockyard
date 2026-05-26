#pragma once

#include <dockyard/mesh.hpp>
#include <dockyard/pipeline_builder.hpp>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtx/quaternion.hpp>

#include <entt/entt.hpp>

#include <dockyard/components.hpp>

namespace dy {

class Scene;
class Entity {
  entt::registry &reg;
  entt::entity entity;

public:
  Entity(entt::registry &r, entt::entity_like auto identifier)
      : reg(r), entity(identifier) {}
  Entity(Scene &, entt::entity);

  explicit Entity(entt::registry &r) : reg(r), entity(reg.create()) {}

  [[nodiscard]] auto valid() const -> bool { return entity != entt::null; }
  [[nodiscard]] auto id() const -> entt::entity { return entity; }
  [[nodiscard]] auto handle() const -> entt::entity { return entity; }

  template <typename T, typename... Args>
  auto emplace(Args &&...args) -> decltype(auto) {
    return reg.emplace<T>(entity, std::forward<Args>(args)...);
  }
  template <typename T> auto get() -> decltype(auto) {
    return reg.get<T>(entity);
  }
  template <typename T> auto try_get() -> T * { return reg.try_get<T>(entity); }
  template <typename T> auto remove() -> void { reg.remove<T>(entity); }
};

class Scene {
  entt::entity primary_camera_entity{entt::null};
  entt::registry scene;

  friend class Entity;

public:
  template <typename... Ts> auto view(auto &&...excludes) -> decltype(auto) {
    return scene.view<Ts...>(std::forward<decltype(excludes)>(excludes)...);
  }

  template <typename... Ts> auto group(auto &&...excludes) -> decltype(auto) {
    return scene.group<Ts...>(std::forward<decltype(excludes)>(excludes)...);
  }

  template <typename T> auto on_construct() { return scene.on_construct<T>(); }
  template <typename T> auto on_destroy() { return scene.on_destroy<T>(); }

  auto registry() -> entt::registry & { return scene; }
  auto registry() const -> const entt::registry & { return scene; }

  auto make(std::string_view, entt::entity = entt::null) -> Entity;
  auto make(std::string_view, Entity &) -> Entity;

  auto make_camera(std::string_view, u32 width, u32 height,
                   glm::vec3 position = {0.0F, 0.0F, 0.0F},
                   glm::vec3 look_at = {0.0F, 0.0F, 1.0F}) -> Entity;

  auto set_primary_camera(entt::entity e) -> void;
  auto clear_primary_camera() -> void;

  [[nodiscard]] auto primary_camera() -> Components::Camera * {
    if (primary_camera_entity == entt::null)
      return nullptr;
    return scene.try_get<Components::Camera>(primary_camera_entity);
  }
};

} // namespace dy

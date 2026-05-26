#include <dockyard/scene.hpp>

namespace dy {

Entity::Entity(Scene &s, entt::entity e) : reg(s.scene), entity(e) {}

auto Scene::make(std::string_view name, entt::entity maybe_parent) -> Entity {
  auto new_entity = Entity{scene};
  new_entity.emplace<Components::Tag>(name);
  new_entity.emplace<Components::Transform>();
  new_entity.emplace<Components::LocalToWorld>();

  if (maybe_parent != entt::null && scene.valid(maybe_parent)) {
    new_entity.emplace<Components::ParentOf>(maybe_parent);
  }

  return new_entity;
}
auto Scene::make(std::string_view name, Entity &entity) -> Entity {
  auto new_entity = Entity{scene};
  new_entity.emplace<Components::Tag>(name);
  new_entity.emplace<Components::Transform>();
  new_entity.emplace<Components::LocalToWorld>();

  if (entity.valid()) {
    new_entity.emplace<Components::ParentOf>(entity.handle());
  }

  return new_entity;
}
auto Scene::make_camera(std::string_view name, u32 width, u32 height,
                        glm::vec3 position, glm::vec3 look_at) -> Entity {
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

auto Scene::set_primary_camera(entt::entity e) -> void {
  primary_camera_entity = e;
}

auto Scene::clear_primary_camera() -> void {
  primary_camera_entity = entt::null;
}

} // namespace dy
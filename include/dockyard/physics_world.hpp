#pragma once

#include <dockyard/types.hpp>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtx/quaternion.hpp>

#include <entt/entity/entity.hpp>

#include <memory>
#include <span>

namespace dy {

class Scene;
struct PositionOnlyVertex;

namespace Components {
struct Collider;
struct RigidBody;
struct CharacterController;
struct Constraint;
} // namespace Components

// LOD0-only CPU geometry for a mesh collider, retained on MeshAsset only when
// requested at load time (see mesh_loader.hpp).
struct MeshCollisionGeometry {
  std::span<const PositionOnlyVertex> positions;
  std::span<const u32> indices;
};

struct BodyTransform {
  glm::vec3 position{0.0F};
  glm::quat rotation{1.0F, 0.0F, 0.0F, 0.0F};
  // Applied to the collision shape via btCollisionShape::setLocalScaling.
  // Non-uniform scale is only well-supported by Bullet for box/mesh shapes;
  // sphere/capsule approximate it as uniform.
  glm::vec3 scale{1.0F};
};

// Owns a Bullet btDiscreteDynamicsWorld and everything registered in it for
// the duration of one Play session. Bullet headers are confined to
// physics_world.cpp (pimpl) so the ECS/component headers stay physics-engine
// agnostic.
class PhysicsWorld {
public:
  PhysicsWorld();
  ~PhysicsWorld();
  PhysicsWorld(const PhysicsWorld &) = delete;
  auto operator=(const PhysicsWorld &) -> PhysicsWorld & = delete;
  PhysicsWorld(PhysicsWorld &&) = delete;
  auto operator=(PhysicsWorld &&) -> PhysicsWorld & = delete;

  auto step(f32 dt) -> void;

  // Writes resolved transforms of dynamic (non-kinematic, non-static) rigid
  // bodies back into Components::Transform for every entity with a RigidBody.
  auto sync_transforms_out(Scene &scene) -> void;

  // mesh_geometry must be non-null and populated when collider.shape ==
  // ColliderShape::Mesh; ignored otherwise. Returns a runtime_id to store on
  // Components::RigidBody / Components::Collider (0 on failure).
  [[nodiscard]] auto create_rigid_body(const Components::Collider &collider,
                                       const Components::RigidBody &body,
                                       const BodyTransform &transform,
                                       const MeshCollisionGeometry *mesh_geometry)
      -> u64;
  auto destroy_rigid_body(u64 runtime_id) -> void;
  auto set_kinematic_transform(u64 runtime_id, const BodyTransform &transform)
      -> void;

  [[nodiscard]] auto create_character_controller(
      const Components::CharacterController &controller,
      const glm::vec3 &position) -> u64;
  auto destroy_character_controller(u64 runtime_id) -> void;
  // walk_direction is a world-space velocity (already scaled by move speed);
  // jump requests a jump impulse for this tick if the controller is grounded.
  auto move_character(u64 runtime_id, const glm::vec3 &walk_direction, bool jump,
                      f32 dt) -> void;
  [[nodiscard]] auto character_position(u64 runtime_id) const -> glm::vec3;
  [[nodiscard]] auto character_on_ground(u64 runtime_id) const -> bool;

  // body_a/body_b are runtime_ids from create_rigid_body; 0 means "the world"
  // (a fixed anchor) for either side.
  [[nodiscard]] auto create_constraint(const Components::Constraint &constraint,
                                       u64 body_a_runtime_id,
                                       u64 body_b_runtime_id) -> u64;
  auto destroy_constraint(u64 runtime_id) -> void;

private:
  struct Impl;
  std::unique_ptr<Impl> impl;
};

} // namespace dy

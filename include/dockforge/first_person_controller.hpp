#pragma once

namespace dy {
class Scene;
class PhysicsWorld;
} // namespace dy

extern "C" {
struct GLFWwindow;
}

namespace first_person_system {

// Applies mouse-look to every Components::Camera + FirstPersonController
// entity. Entities that also have a Components::CharacterController compute a
// walk direction from WASD and push it into the physics world's kinematic
// capsule (PhysicsWorld::move_character) ahead of PhysicsWorld::step();
// entities without one move the camera directly (free-fly, no collision —
// useful for no-physics debugging).
auto pre_physics_update(dy::Scene &scene, dy::PhysicsWorld &physics_world,
                        GLFWwindow *window, float ts) -> void;

// Call after PhysicsWorld::step(). Copies each CharacterController's resolved
// capsule position back onto its Camera (offset up to eye height).
auto post_physics_update(dy::Scene &scene, const dy::PhysicsWorld &physics_world)
    -> void;

} // namespace first_person_system

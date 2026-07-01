#include <dockforge/first_person_controller.hpp>

#include <dockyard/components.hpp>
#include <dockyard/physics_world.hpp>
#include <dockyard/scene.hpp>

#include <GLFW/glfw3.h>

namespace first_person_system {

using namespace dy;

auto pre_physics_update(Scene &scene, PhysicsWorld &physics_world,
                        GLFWwindow *window, float ts) -> void {
  for (auto &&[entity, cam, ctrl] :
       scene.view<Components::Camera, Components::FirstPersonController>()
           .each()) {
    cam.yaw += glm::radians(ctrl.pending_dx * ctrl.sensitivity);
    cam.pitch =
        glm::clamp(cam.pitch - glm::radians(ctrl.pending_dy * ctrl.sensitivity),
                   glm::radians(-89.0F), glm::radians(89.0F));
    ctrl.pending_dx = 0.0F;
    ctrl.pending_dy = 0.0F;

    glm::vec3 dir{0.0F};
    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS)
      dir += cam.forward();
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS)
      dir -= cam.forward();
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS)
      dir += cam.right();
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS)
      dir -= cam.right();

    auto *char_ctrl =
        scene.registry().try_get<Components::CharacterController>(entity);
    if (char_ctrl == nullptr) {
      // No CharacterController: free-fly, no collision (debug path).
      constexpr glm::vec3 world_up{0.0F, 1.0F, 0.0F};
      if (glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS)
        dir += world_up;
      if (glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS)
        dir -= world_up;
      if (glm::length(dir) > 1e-6F)
        cam.position += glm::normalize(dir) * ctrl.move_speed * ts;
      continue;
    }

    if (char_ctrl->runtime_id == 0)
      continue;

    // Flatten to the horizontal plane: looking up/down shouldn't change walk
    // speed, and vertical motion is driven by gravity/jump, not look pitch.
    dir.y = 0.0F;
    glm::vec3 walk_direction{0.0F};
    if (glm::length(dir) > 1e-6F)
      walk_direction = glm::normalize(dir) * char_ctrl->move_speed;

    const bool jump = glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS;
    physics_world.move_character(char_ctrl->runtime_id, walk_direction, jump, ts);
  }
}

auto post_physics_update(Scene &scene, const PhysicsWorld &physics_world)
    -> void {
  // Eyes sit near, but not exactly at, the top of the capsule's head.
  constexpr f32 eye_offset_ratio = 0.9F;

  for (auto &&[entity, cam, char_ctrl] :
       scene.view<Components::Camera, Components::CharacterController>()
           .each()) {
    if (char_ctrl.runtime_id == 0)
      continue;

    const glm::vec3 capsule_center =
        physics_world.character_position(char_ctrl.runtime_id);
    const f32 half_extent = (char_ctrl.height * 0.5F) + char_ctrl.radius;
    cam.position =
        capsule_center + glm::vec3{0.0F, half_extent * eye_offset_ratio, 0.0F};
  }
}

} // namespace first_person_system

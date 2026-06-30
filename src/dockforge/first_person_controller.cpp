#include <dockforge/first_person_controller.hpp>

#include <dockyard/components.hpp>
#include <dockyard/scene.hpp>

#include <GLFW/glfw3.h>

namespace first_person_system {

auto update(dy::Scene &scene, GLFWwindow *window, float ts) -> void {
  using namespace dy;

  for (auto &&[entity, cam, ctrl] :
       scene.view<Components::Camera, Components::FirstPersonController>()
           .each()) {
    cam.yaw += glm::radians(ctrl.pending_dx * ctrl.sensitivity);
    cam.pitch =
        glm::clamp(cam.pitch - glm::radians(ctrl.pending_dy * ctrl.sensitivity),
                   glm::radians(-89.0F), glm::radians(89.0F));
    ctrl.pending_dx = 0.0F;
    ctrl.pending_dy = 0.0F;

    constexpr glm::vec3 world_up{0.0F, 1.0F, 0.0F};
    glm::vec3 dir{0.0F};
    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS)
      dir += cam.forward();
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS)
      dir -= cam.forward();
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS)
      dir += cam.right();
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS)
      dir -= cam.right();
    if (glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS)
      dir += world_up;
    if (glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS)
      dir -= world_up;

    if (glm::length(dir) > 1e-6F)
      cam.position += glm::normalize(dir) * ctrl.move_speed * ts;
  }
}

} // namespace first_person_system

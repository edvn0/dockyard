#include <dockforge/free_camera_controller.hpp>

#include <dockyard/scene.hpp>

#include <GLFW/glfw3.h>
#include <imgui.h>

FreeCameraController::FreeCameraController(Components::Camera &cam,
                                           GLFWwindow *w)
    : camera(cam), window(w) {}

auto FreeCameraController::update(float ts) -> void {
  if (ImGui::GetIO().WantCaptureKeyboard)
    return;

  auto fwd = camera.forward();
  auto right = camera.right();
  constexpr glm::vec3 world_up{0.0f, 1.0f, 0.0f};

  glm::vec3 dir{0.0f};
  if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS)
    dir += fwd;
  if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS)
    dir -= fwd;
  if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS)
    dir += right;
  if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS)
    dir -= right;
  if (glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS)
    dir += world_up;
  if (glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS)
    dir -= world_up;

  if (glm::length(dir) > 1e-6f)
    camera.position += glm::normalize(dir) * move_speed * ts;
}
auto FreeCameraController::on_mouse_delta(float dx, float dy) -> void {
  if (ImGui::GetIO().WantCaptureMouse)
    return;

  camera.yaw += glm::radians(dx * sensitivity);
  camera.pitch = glm::clamp(camera.pitch - glm::radians(dy * sensitivity),
                            glm::radians(-89.0f), glm::radians(89.0f));
}

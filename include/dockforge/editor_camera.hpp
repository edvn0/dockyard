#pragma once
#include <dockforge/free_camera_controller.hpp>
#include <dockforge/path_camera_controller.hpp>

struct EditorCamera {
  EditorCamera(const EditorCamera &) = delete;
  EditorCamera &operator=(const EditorCamera &) = delete;
  EditorCamera(EditorCamera &&) = delete;
  EditorCamera &operator=(EditorCamera &&) = delete;

  Components::Camera camera{};
  FreeCameraController free_controller;
  PathCameraController path_controller;

  bool use_path = false;

  EditorCamera(GLFWwindow *w, glm::vec3 position, glm::vec3 look_at, u32 width,
               u32 height)
      : free_controller(camera, w), path_controller(camera) {
    camera.position = position;
    camera.set_aspect(width, height);
    auto [y, p] = Components::Camera::facing_toward(position, look_at);
    camera.yaw = y;
    camera.pitch = p;
  }

  auto save_keyframe(float time_to_reach) -> void {
    glm::vec3 fwd = camera.forward();
    glm::quat rot = glm::quatLookAtLH(fwd, glm::vec3(0, 1, 0));
    path_controller.path.push_back(
        {camera.position, rot, time_to_reach, EaseType::SmoothStep});
  }

  auto update(float ts) -> void {
    if (use_path)
      path_controller.update(ts);
    else
      free_controller.update(ts);
  }

  auto on_mouse_delta(float dx, float dy) -> void {
    if (!use_path)
      free_controller.on_mouse_delta(dx, dy);
  }

  auto set_aspect(u32 w, u32 h) -> void { camera.set_aspect(w, h); }
  [[nodiscard]] auto view() const -> glm::mat4 { return camera.view(); }
  [[nodiscard]] auto projection() const -> glm::mat4 {
    return camera.projection();
  }
};

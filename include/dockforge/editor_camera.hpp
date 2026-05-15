#pragma once

#include <dockforge/free_camera_controller.hpp>
#include <dockforge/path_camera_controller.hpp>

#include <dockyard/scene.hpp>

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
               u32 height);

  auto save_keyframe(float time_to_reach) -> void;
  auto update(float ts) -> void;
  auto on_mouse_delta(float dx, float dy) -> void;
  auto set_aspect(u32 w, u32 h) -> void;
  auto set_pose(glm::vec3 position, glm::vec3 look_at) -> void;
  [[nodiscard]] auto view() const -> glm::mat4;
  [[nodiscard]] auto projection() const -> glm::mat4;
  [[nodiscard]] auto position() const -> glm::vec3;
  [[nodiscard]] auto near_plane() const -> f32;
  [[nodiscard]] auto far_plane() const -> f32;
};

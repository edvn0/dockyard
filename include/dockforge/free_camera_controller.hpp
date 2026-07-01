#pragma once

extern "C" {
struct GLFWwindow;
}

namespace dy::Components {
struct Camera;
}

using namespace dy;

struct FreeCameraController {
  Components::Camera &camera;
  GLFWwindow *window = nullptr;
  // The editor camera is a free-fly inspection tool, not a to-scale
  // character — it needs to cross a whole scene quickly regardless of how
  // large that scene is (1 unit == 1 meter), so it defaults much faster than
  // Components::CharacterController's realistic human walk speed.
  float move_speed = 30.0f;
  float sensitivity = 0.1f;

  explicit FreeCameraController(Components::Camera &cam, GLFWwindow *w);
  auto update(float ts) -> void;
  auto on_mouse_delta(float dx, float dy) -> void;
};

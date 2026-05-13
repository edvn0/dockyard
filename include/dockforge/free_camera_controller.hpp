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
  float move_speed = 5.0f;
  float sensitivity = 0.1f;

  explicit FreeCameraController(Components::Camera &cam, GLFWwindow *w);
  auto update(float ts) -> void;
  auto on_mouse_delta(float dx, float dy) -> void;
};

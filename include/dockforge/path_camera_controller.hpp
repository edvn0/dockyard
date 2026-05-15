#pragma once

#include <dockyard/types.hpp>
#include <glm/glm.hpp>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>

extern "C" {
struct GLFWwindow;
}

namespace dy::Components {
struct Camera;
}

using namespace dy;

enum class EaseType { Linear, SmoothStep, InOutSine };

struct CameraKeyframe {
  glm::vec3 position;
  glm::quat orientation;
  float travel_time;
  EaseType easing = EaseType::SmoothStep;
};

struct PathCameraController {
  Components::Camera &camera;
  std::vector<CameraKeyframe> path;

  usize current_index = 0;
  float segment_time = 0.0f;
  bool is_playing = false;

  float apply_easing(float t, EaseType type);
  void update(float ts);
  void update_camera_from_quat(const glm::quat &q);
};
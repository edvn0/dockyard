#include <dockforge/path_camera_controller.hpp>

#include <dockyard/components.hpp>

float PathCameraController::apply_easing(float t, EaseType type) {
  switch (type) {
  case EaseType::SmoothStep:
    return t * t * (3.0f - 2.0f * t);
  case EaseType::InOutSine:
    return -(glm::cos(glm::pi<float>() * t) - 1.0f) / 2.0f;
  default:
    return t;
  }
}

void PathCameraController::update(float ts) {
  if (!is_playing || current_index >= path.size() - 1) {
    camera.forward_override = std::nullopt;
    return;
  }

  const auto &start = path[current_index];
  const auto &end = path[current_index + 1];

  segment_time += ts;
  float linear_t = glm::clamp(segment_time / end.travel_time, 0.0f, 1.0f);
  float eased_t = apply_easing(linear_t, end.easing);

  camera.position = glm::mix(start.position, end.position, eased_t);

  glm::quat current_rot =
      glm::slerp(start.orientation, end.orientation, eased_t);

  glm::vec3 path_forward = current_rot * glm::vec3(0.0f, 0.0f, 1.0f);
  camera.forward_override = glm::normalize(path_forward);

  if (linear_t >= 1.0f) {
    current_index++;
    segment_time = 0.0f;

    if (current_index >= path.size() - 1) {
      auto [y, p] = Components::Camera::facing_toward(glm::vec3(0),
                                                      *camera.forward_override);
      camera.yaw = y;
      camera.pitch = p;
      camera.forward_override = std::nullopt;
      is_playing = false;
    }
  }
}
void PathCameraController::update_camera_from_quat(const glm::quat &q) {
  glm::vec3 forward = q * glm::vec3(0, 0, -1);
  auto [y, p] = Components::Camera::facing_toward(glm::vec3(0.0f), forward);
  camera.yaw = y;
  camera.pitch = p;
}

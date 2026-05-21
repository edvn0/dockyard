#include <dockforge/editor_camera.hpp>

EditorCamera::EditorCamera(GLFWwindow *w, glm::vec3 position, glm::vec3 look_at,
                           u32 width, u32 height)
    : free_controller{camera, w}, path_controller{.camera = camera} {
  camera.position = position;
  camera.set_aspect(width, height);
  auto [y, p] = Components::Camera::facing_toward(position, look_at);
  camera.yaw = y;
  camera.pitch = p;
}

auto EditorCamera::save_keyframe(float time_to_reach) -> void {
  glm::vec3 fwd = camera.forward();
  glm::quat rot = glm::quatLookAtLH(fwd, glm::vec3(0, 1, 0));
  path_controller.path.push_back({
      .position = camera.position,
      .orientation = rot,
      .travel_time = time_to_reach,
      .easing = EaseType::SmoothStep,
  });
}

auto EditorCamera::update(float ts) -> void {
  if (use_path)
    path_controller.update(ts);
  else
    free_controller.update(ts);
}

auto EditorCamera::on_mouse_delta(float dx, float dy) -> void {
  if (!use_path)
    free_controller.on_mouse_delta(dx, dy);
}

auto EditorCamera::set_aspect(u32 w, u32 h) -> void { camera.set_aspect(w, h); }

auto EditorCamera::set_pose(glm::vec3 position, glm::vec3 look_at) -> void {
  camera.position = position;
  auto [y, p] = Components::Camera::facing_toward(position, look_at);
  camera.yaw = y;
  camera.pitch = p;
}

[[nodiscard]] auto EditorCamera::view() const -> glm::mat4 {
  return camera.view();
}

[[nodiscard]] auto EditorCamera::projection() const -> glm::mat4 {
  return camera.projection();
}

[[nodiscard]] auto EditorCamera::position() const -> glm::vec3 {
  return camera.position;
}

[[nodiscard]] auto EditorCamera::near_plane() const -> f32 {
  return camera.near_plane;
}

[[nodiscard]] auto EditorCamera::far_plane() const -> f32 {
  return camera.far_plane;
}

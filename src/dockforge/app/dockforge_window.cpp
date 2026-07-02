#include <dockforge/dockforge.hpp>

#include <dockforge/editor_camera.hpp>
#include <dockforge/editor_utils.hpp>

#include <dockyard/components.hpp>
#include <dockyard/scene_renderer.hpp>

using namespace dy;

auto Dockforge::on_mouse_moved(const events::MouseMoved &e) -> void {
  if (sim_state.in<sim::S::Playing>()) {
    for (auto &&[entity, ctrl] :
         active_scene->view<Components::FirstPersonController>().each()) {
      ctrl.pending_dx += e.dx;
      ctrl.pending_dy += e.dy;
    }
    return;
  }
  if (glfwGetMouseButton(App::get_window(), GLFW_MOUSE_BUTTON_RIGHT) ==
      GLFW_PRESS)
    editor_camera->on_mouse_delta(e.dx, e.dy);
}

auto Dockforge::on_mouse_scrolled(const events::MouseScrolled &e) -> void {
  if (!viewport_hovered)
    return;
  editor_camera->on_mouse_scrolled(e);
}

auto Dockforge::on_key_released(const events::KeyReleased &e) -> void {
  if (e.key == GLFW_KEY_ESCAPE && !sim_state.in<sim::S::Editing>()) {
    stop();
    return;
  }

  if (e.key == GLFW_KEY_F2 && e.mods == GLFW_MOD_SHIFT)
    editor_camera->save_keyframe(2.F);

  if (e.key == GLFW_KEY_F3 && e.mods == GLFW_MOD_SHIFT) {
    editor_camera->use_path = !editor_camera->use_path;
    if (editor_camera->use_path) {
      editor_camera->path_controller.current_index = 0;
      editor_camera->path_controller.segment_time = 0.0F;
      editor_camera->path_controller.is_playing = true;
    }
  }

  if (e.key == GLFW_KEY_F && e.mods == (GLFW_MOD_ALT | GLFW_MOD_SHIFT)) {
    toggle_fullscreen(FullscreenMode::borderless);
  }
  if (e.key == GLFW_KEY_F &&
      e.mods == (GLFW_MOD_CONTROL | GLFW_MOD_ALT | GLFW_MOD_SHIFT)) {
    toggle_fullscreen(FullscreenMode::exclusive);
  }

  if (e.key == GLFW_KEY_F12)
    capture_screenshot();
}

auto Dockforge::resolve_camera() const -> std::pair<glm::mat4, glm::mat4> {
  if (auto *cam = active_scene->primary_camera())
    return {cam->view(), cam->projection()};
  return {editor_camera->view(), editor_camera->projection()};
}

auto Dockforge::resolve_camera_with_position() const
    -> std::tuple<glm::mat4, glm::mat4, glm::vec3> {
  if (auto *cam = active_scene->primary_camera())
    return {cam->view(), cam->projection(), cam->position};
  return {editor_camera->view(), editor_camera->projection(),
          editor_camera->position()};
}

auto Dockforge::resize(u32 w, u32 h) -> void {
  trace("Dockforge resized to {}x{}", w, h);
  viewport_resources.resize(*context, *renderer, w, h);
  renderer->resize();
  editor_camera->set_aspect(w, h);
  for (auto &&[e, cam] : active_scene->view<Components::Camera>().each())
    cam.set_aspect(w, h);
}

auto Dockforge::try_pick_entity(glm::vec2 mouse_screen) -> void {
  auto [view, proj] = resolve_camera();
  const auto ray = screen_to_ray(
      mouse_screen,
      glm::vec2(viewport_panel_offset.width, viewport_panel_offset.height),
      glm::vec2(viewport_panel_extent.width, viewport_panel_extent.height),
      view, proj);

  entt::entity best = entt::null;
  float best_t = std::numeric_limits<float>::max();

  for (auto &&[e, ltw, m] :
       active_scene->view<Components::LocalToWorld, Components::Mesh>()
           .each()) {
    const MeshAsset *asset = renderer->get_mesh(m);
    if ((asset == nullptr) || !asset->mesh_aabb.is_valid())
      continue;

    const auto world_aabb = asset->mesh_aabb.transform(ltw.matrix);
    const auto t = ray_aabb(ray, world_aabb.get_min(), world_aabb.get_max());

    if (t >= 0.0F && t < best_t) {
      best_t = t;
      best = e;
    }
  }

  editor_state.selected = best;
}

namespace {
auto find_monitor_for_window(GLFWwindow *window) -> GLFWmonitor * {
  int win_x = 0;
  int win_y = 0;
  int win_w = 0;
  int win_h = 0;
  glfwGetWindowPos(window, &win_x, &win_y);
  glfwGetWindowSize(window, &win_w, &win_h);
  const int center_x = win_x + win_w / 2;
  const int center_y = win_y + win_h / 2;

  int monitor_count = 0;
  GLFWmonitor **monitors = glfwGetMonitors(&monitor_count);

  for (int i = 0; i < monitor_count; ++i) {
    int mon_x = 0;
    int mon_y = 0;
    const GLFWvidmode *mode = glfwGetVideoMode(monitors[i]);
    glfwGetMonitorPos(monitors[i], &mon_x, &mon_y);
    if (center_x >= mon_x && center_x < mon_x + mode->width &&
        center_y >= mon_y && center_y < mon_y + mode->height)
      return monitors[i];
  }
  return glfwGetPrimaryMonitor();
}

auto resolve_target_monitor(GLFWwindow *window, int monitor_index)
    -> GLFWmonitor * {
  if (monitor_index < 0)
    return find_monitor_for_window(window);

  int monitor_count = 0;
  GLFWmonitor **monitors = glfwGetMonitors(&monitor_count);
  if (monitor_index >= monitor_count) {
    warn("Fullscreen: monitor index {} out of range ({} available), "
         "falling back to current monitor",
         monitor_index, monitor_count);
    return find_monitor_for_window(window);
  }
  return monitors[monitor_index];
}
} // namespace

auto Dockforge::list_monitors() -> std::vector<std::string> {
  int count = 0;
  GLFWmonitor **monitors = glfwGetMonitors(&count);
  std::vector<std::string> names;
  names.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i)
    names.emplace_back(glfwGetMonitorName(monitors[i]));
  return names;
}

auto Dockforge::toggle_fullscreen(FullscreenMode mode, int monitor_index)
    -> void {
  auto *glfw_window = App::get_window();

  // Same mode requested while already in it -> drop back to windowed.
  if (is_fullscreen && fullscreen_mode == mode) {
    glfwSetWindowMonitor(glfw_window, nullptr, windowed_pos_x, windowed_pos_y,
                         windowed_width, windowed_height, GLFW_DONT_CARE);
    is_fullscreen = false;
    return;
  }

  if (!is_fullscreen) {
    glfwGetWindowPos(glfw_window, &windowed_pos_x, &windowed_pos_y);
    glfwGetWindowSize(glfw_window, &windowed_width, &windowed_height);
  }

  GLFWmonitor *monitor = resolve_target_monitor(glfw_window, monitor_index);
  const GLFWvidmode *vid_mode = glfwGetVideoMode(monitor);
  int monitor_x = 0;
  int monitor_y = 0;
  glfwGetMonitorPos(monitor, &monitor_x, &monitor_y);

  if (mode == FullscreenMode::exclusive) {
    // Passing a real GLFWmonitor* triggers an actual display-mode switch
    // (DEVMODE change on Win32, XRandR mode set on X11). Refresh rate
    // doesn't need to be an exact match — GLFW snaps to the closest mode.
    glfwSetWindowMonitor(glfw_window, monitor, 0, 0, vid_mode->width,
                         vid_mode->height, vid_mode->refreshRate);
  } else {
    glfwSetWindowMonitor(glfw_window, nullptr, monitor_x, monitor_y,
                         vid_mode->width, vid_mode->height, GLFW_DONT_CARE);
  }

  fullscreen_mode = mode;
  is_fullscreen = true;

  vkDeviceWaitIdle(context->device);
}

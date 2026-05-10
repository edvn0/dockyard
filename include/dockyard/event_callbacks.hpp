#pragma once

#include <dockyard/events.hpp>
#include <dockyard/types.hpp>
#include <entt/entt.hpp>

extern "C" {
struct GLFWwindow;
}

namespace dy {

struct ResizeState {
  bool pending = false;
  u32 width = 0;
  u32 height = 0;
  double last_event_time = 0.0; // from glfwGetTime()
};

struct MouseState {
  float x{};
  float y{};
  bool initialised{false};
};

struct WindowContext {
  entt::dispatcher *dispatcher{};
  ResizeState resize{};
  MouseState mouse{};
};

auto install_glfw_callbacks(GLFWwindow *window, entt::dispatcher &) -> void;
auto uninstall_glfw_callbacks(GLFWwindow *window) -> void;
auto poll_pending_events() -> void;

} // namespace dy
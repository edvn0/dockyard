#include <dockyard/event_callbacks.hpp>

#include <GLFW/glfw3.h>
#include <dockyard/log.hpp>

namespace dy {
namespace {
auto access(GLFWwindow *w) -> WindowContext & {
  auto *ctx = static_cast<WindowContext *>(glfwGetWindowUserPointer(w));
  if (!ctx) {
    error("GLFW callback invoked but no context found");
    std::abort();
  }
  return *ctx;
}
std::unique_ptr<WindowContext> c;
} // namespace

auto install_glfw_callbacks(GLFWwindow *window, entt::dispatcher &dispatcher)
    -> void {
  c = std::make_unique<WindowContext>(WindowContext{
      .dispatcher = &dispatcher,
      .resize = {},
  });
  glfwSetWindowUserPointer(window, c.get());

  glfwSetFramebufferSizeCallback(window,
                                 [](GLFWwindow *w, int width, int height) {
                                   auto &ctx = access(w);
                                   ctx.resize.pending = true;
                                   ctx.resize.width = static_cast<u32>(width);
                                   ctx.resize.height = static_cast<u32>(height);
                                 });

  glfwSetWindowCloseCallback(window, [](GLFWwindow *w) {
    auto &ctx = access(w);
    ctx.dispatcher->enqueue<events::window_closed>();
  });

  glfwSetWindowIconifyCallback(window, [](GLFWwindow *w, int iconified) {
    auto &ctx = access(w);
    ctx.dispatcher->enqueue<events::window_minimized>(iconified == GLFW_TRUE);
  });

  glfwSetKeyCallback(
      window, [](GLFWwindow *w, int key, int scancode, int action, int mods) {
        auto &ctx = access(w);
        if (action == GLFW_PRESS)
          ctx.dispatcher->enqueue<events::key_pressed>(key, scancode, mods);
        else if (action == GLFW_RELEASE)
          ctx.dispatcher->enqueue<events::key_released>(key, scancode, mods);
      });

  glfwSetMouseButtonCallback(
      window, [](GLFWwindow *w, int button, int action, int mods) {
        auto &ctx = access(w);
        if (action == GLFW_PRESS)
          ctx.dispatcher->enqueue<events::mouse_button_pressed>(button, mods);
        else if (action == GLFW_RELEASE)
          ctx.dispatcher->enqueue<events::mouse_button_released>(button, mods);
      });

  glfwSetCursorPosCallback(window, [](GLFWwindow *w, double x, double y) {
    auto &ctx = access(w);
    const float fx = static_cast<float>(x);
    const float fy = static_cast<float>(y);
    const float dx = ctx.mouse.initialised ? fx - ctx.mouse.x : 0.0f;
    const float dy = ctx.mouse.initialised ? fy - ctx.mouse.y : 0.0f;
    ctx.mouse = {fx, fy, true};
    ctx.dispatcher->enqueue<events::mouse_moved>(fx, fy, dx, dy);
  });

  glfwSetScrollCallback(window, [](GLFWwindow *w, double dx, double dy) {
    auto &ctx = access(w);
    ctx.dispatcher->enqueue<events::mouse_scrolled>(static_cast<float>(dx),
                                                    static_cast<float>(dy));
  });
}

auto poll_pending_events() -> void {
  if (!c || !c->resize.pending)
    return;

  c->resize.pending = false;
  if (c->resize.width == 0 || c->resize.height == 0)
    return;

  c->dispatcher->enqueue<events::swapchain_invalidated>(c->resize.width,
                                                        c->resize.height);
}

auto uninstall_glfw_callbacks(GLFWwindow *window) -> void {
  c.reset();
  glfwSetWindowUserPointer(window, nullptr);
}
} // namespace dy
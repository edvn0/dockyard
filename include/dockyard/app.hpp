#pragma once

#include "dockyard/events.hpp"
#include <dockyard/context.hpp>
#include <dockyard/types.hpp>

#include <tuple>

extern "C" {
struct GLFWwindow;
}

namespace dy {

struct RenderContext;

struct InitialisationContext {
  std::tuple<u32, u32> window_extent;
  std::tuple<u32, u32> viewport_extent;
  VulkanContext &context;
  SwapchainResources &swapchain_resources;
};

struct DeletionQueue {
  using Fn = std::function<void()>;

  static auto get() -> DeletionQueue & {
    static DeletionQueue instance;
    return instance;
  }

  auto begin_frame(std::unsigned_integral auto frame_index) -> void {
    current_frame = static_cast<u32>(frame_index);
    flush(static_cast<u32>(frame_index));
  }

  auto push(Fn fn) -> void {
    per_frame[current_frame].push_back(std::move(fn));
  }

  auto flush_all() {
    std::ranges::for_each(per_frame, [](auto &v) {
      std::ranges::for_each(v, [](auto &k) { k(); });
    });
  }

private:
  std::array<std::vector<Fn>, frames_in_flight> per_frame{};
  u32 current_frame = 0u;
  auto flush(u32 frame_index) -> void {
    for (auto &fn : per_frame[frame_index])
      fn();
    per_frame[frame_index].clear();
  }

  DeletionQueue() = default;
  DeletionQueue(const DeletionQueue &) = delete;
  DeletionQueue &operator=(const DeletionQueue &) = delete;
  DeletionQueue(DeletionQueue &&) = delete;
  DeletionQueue &operator=(DeletionQueue &&) = delete;
};

class App {
public:
  virtual ~App() = default;
  auto run(i32 argc, char *argv[]) -> i32;

  virtual auto init(const InitialisationContext &) -> void = 0;
  virtual auto update(float ts) -> void = 0;
  virtual auto resize(u32, u32) -> void = 0;
  virtual auto render(RenderContext &) -> u64 = 0;
  virtual auto destroy() -> void = 0;

  virtual auto on_key_pressed(const events::key_pressed &) -> void {}
  virtual auto on_key_released(const events::key_released &) -> void {}
  virtual auto on_mouse_button_pressed(const events::mouse_button_pressed &)
      -> void {}
  virtual auto on_mouse_button_released(const events::mouse_button_released &)
      -> void {}
  virtual auto on_mouse_moved(const events::mouse_moved &) -> void {}
  virtual auto on_mouse_scrolled(const events::mouse_scrolled &) -> void {}

protected:
  auto get_window() const { return window; }

private:
  GLFWwindow *window;

  auto recreate_swapchain_manually(GLFWwindow *, const RendererListener &)
      -> void;
  auto on_swapchain_resized(const events::swapchain_resized &) -> void;
};
} // namespace dy
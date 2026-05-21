#pragma once

#include <dockyard/context.hpp>
#include <dockyard/events.hpp>
#include <dockyard/types.hpp>

#include <tuple>

extern "C" {
struct GLFWwindow;
}

namespace dy {

#ifdef DOCKYARD_DEBUG
inline constexpr auto enable_validation_layers = true;
#else
inline constexpr auto enable_validation_layers = false;
#endif

struct RenderContext;

struct InitialisationContext {
  std::tuple<u32, u32> window_extent;
  std::tuple<u32, u32> viewport_extent;
  VulkanContext &context;
  SwapchainResources &swapchain_resources;
};

struct DeletionQueue {
  DeletionQueue(const DeletionQueue &) = delete;
  DeletionQueue &operator=(const DeletionQueue &) = delete;
  DeletionQueue(DeletionQueue &&) = delete;
  DeletionQueue &operator=(DeletionQueue &&) = delete;

  static auto the() -> DeletionQueue &;

  auto begin_frame(std::unsigned_integral auto frame_index) -> void {
    current_frame = static_cast<u32>(frame_index);
    flush(static_cast<u32>(frame_index));
  }

  using Fn = std::function<void()>;
  auto push(Fn &&) -> void;
  auto on_destroy(Fn &&) -> void;
  auto flush_all() -> void;

  auto destroy_at_exit(VkDevice, VkPipeline, VkPipelineLayout) -> void;

private:
  std::array<std::vector<Fn>, frames_in_flight> per_frame{};
  std::vector<Fn> on_app_exit;
  u32 current_frame = 0U;
  auto flush(u32 frame_index) -> void;

  DeletionQueue() = default;
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

  virtual auto on_key_pressed(const events::KeyPressed &) -> void {}
  virtual auto on_key_released(const events::KeyReleased &) -> void {}
  virtual auto on_mouse_button_pressed(const events::MouseButtonPressed &)
      -> void {}
  virtual auto on_mouse_button_released(const events::MouseButtonReleased &)
      -> void {}
  virtual auto on_mouse_moved(const events::MouseMoved &) -> void {}
  virtual auto on_mouse_scrolled(const events::MouseScrolled &) -> void {}

protected:
  [[nodiscard]] auto get_window() const { return window; }
  [[nodiscard]] auto get_frame_index() const -> const auto & {
    return frame_index;
  }
  [[nodiscard]] auto get_frame_index_mut() -> auto & { return frame_index; }

private:
  GLFWwindow *window;
  u64 frame_index = 0;
  u64 last_frame_index = 0;

  static auto recreate_swapchain_manually(GLFWwindow *,
                                          const RendererListener &) -> void;
  auto on_swapchain_resized(const events::SwapchainResized &) -> void;
};
} // namespace dy
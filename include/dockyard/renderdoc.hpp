#pragma once

#include <memory>
#include <string_view>

struct RenderDocApi;

struct RenderDocContext {
  std::unique_ptr<RenderDocApi> api{};

  RenderDocContext() = default;
  ~RenderDocContext();

  RenderDocContext(RenderDocContext &&) noexcept;
  auto operator=(RenderDocContext &&) noexcept -> RenderDocContext &;

  RenderDocContext(const RenderDocContext &) = delete;
  auto operator=(const RenderDocContext &) -> RenderDocContext & = delete;

  [[nodiscard]] auto is_active() const -> bool { return api != nullptr; }
  [[nodiscard]] auto is_capturing() const -> bool;

  auto begin_frame_capture(void *vk_instance, void *wnd_handle = nullptr) const
      -> void;
  auto end_frame_capture(void *vk_instance, void *wnd_handle = nullptr) const
      -> void;

  auto trigger_capture() const -> void;
  auto set_capture_path(std::string_view) const -> void;
};

[[nodiscard]] auto renderdoc_init() -> RenderDocContext;

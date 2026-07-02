#pragma once

#include <dockyard/buffer.hpp>
#include <dockyard/context.hpp>
#include <dockyard/texture.hpp>
#include <dockyard/types.hpp>

#include <expected>
#include <filesystem>
#include <memory>
#include <string>

namespace dy {

// GPU->CPU readback state for an in-flight screenshot capture. The copy is
// recorded into the caller's command buffer; the pixels are not valid to read
// until that command buffer's GPU work has completed.
struct ScreenshotReadback {
  std::unique_ptr<Buffer> staging;
  u32 width{};
  u32 height{};
  std::filesystem::path output_path;
};

// Records a copy of `source` (must be VK_FORMAT_R8G8B8A8_SRGB or _UNORM, kept
// in VK_IMAGE_LAYOUT_GENERAL) into a freshly-allocated host-visible staging
// buffer, as part of `cmd`. The caller must defer write_screenshot_png() until
// the GPU work in `cmd` has completed, e.g. via DeletionQueue::the().push(...).
[[nodiscard]] auto record_screenshot_copy(const VulkanContext &ctx,
                                          VkCommandBuffer cmd,
                                          const Texture &source,
                                          std::filesystem::path output_path)
    -> std::expected<ScreenshotReadback, std::string>;

// Encodes the captured pixels as a PNG at `readback.output_path`. Must only be
// called once the GPU copy recorded by record_screenshot_copy() has completed.
auto write_screenshot_png(const ScreenshotReadback &readback) -> void;

} // namespace dy

#pragma once

#include <dockyard/bindless_descriptor.hpp>

#include <dockyard/forward.hpp>

#include <span>

namespace dy::pool {

struct CpuTextureData {
  std::vector<std::byte> pixels;
  std::string name;
  std::string cache_key;
  u32 width{};
  u32 height{};
  VkFormat format{VK_FORMAT_R8G8B8A8_SRGB};
  bool generate_mips{false};
  bool storage_view{false};

  [[nodiscard]] auto to_create_info() const -> Texture::CreateInfo {
    return {
        .bytes = std::span{pixels},
        .width = width,
        .height = height,
        .format = format,
        .generate_mips = generate_mips,
        .storage_view = storage_view,
    };
  }
};

struct PendingUpload {
  std::future<CpuTextureData> cpu_work;
  std::function<void(TextureHandle)> on_complete;
  std::stop_source stop_src;
};

class TextureUploadPool {
public:
  auto submit(std::future<CpuTextureData> fut,
              std::stop_source stop_src, // <--- Pass this in from loader
              std::function<void(TextureHandle)> on_complete) {
    std::scoped_lock lock{work_mutex};
    if (stopped.load(std::memory_order_relaxed)) {
      stop_src.request_stop(); // Kill it immediately if pool is dead
      return;
    }

    pending_work.push_back({.cpu_work = std::move(fut),
                            .on_complete = std::move(on_complete),
                            .stop_src = std::move(stop_src)});
  }

  auto poll_one(SceneRenderer &renderer) -> void;
  [[nodiscard]] bool empty() const;
  auto drop() -> void;

private:
  mutable std::mutex work_mutex;
  std::vector<PendingUpload> pending_work;
  std::atomic<bool> stopped{false};
};

} // namespace dy::pool

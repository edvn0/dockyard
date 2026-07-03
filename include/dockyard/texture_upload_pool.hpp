#pragma once

#include <dockyard/bindless_descriptor.hpp>
#include <dockyard/upload_pool.hpp>

#include <dockyard/forward.hpp>

#include <span>

namespace dy::pool {

struct MipData {
  std::vector<std::byte> pixels;
  u32 width{};
  u32 height{};
};

struct CpuTextureData {
  std::span<const std::byte> pixels;
  std::vector<MipData> mips;
  std::string name;
  std::string cache_key;
  u32 width{};
  u32 height{};
  VkFormat format{VK_FORMAT_R8G8B8A8_SRGB};
  bool generate_mips{false};
  bool storage_view{false};

  [[nodiscard]] auto to_create_info() const -> Texture::CreateInfo {
    std::span<const std::byte> base_bytes{};
    std::span<const MipData> sub_mips{};

    if (!pixels.empty()) {
      base_bytes = pixels;
      sub_mips = std::span<const MipData>{}; // No pre-baked mips
    } else if (!mips.empty()) {
      base_bytes = mips[0].pixels;
      if (mips.size() > 1) {
        sub_mips = std::span{mips}.subspan(1);
      }
    }

    return {
        .bytes = base_bytes,
        .mips = sub_mips,
        .width = width,
        .height = height,
        .format = format,
        .generate_mips = generate_mips,
        .storage_view = storage_view,
    };
  }
};

class TextureUploadPool : public UploadPool<CpuTextureData, TextureHandle> {
public:
  auto poll_one(SceneRenderer &renderer) -> void { poll_n(renderer, 1); }
  auto poll_n(SceneRenderer &renderer, usize) -> void;
};

} // namespace dy::pool

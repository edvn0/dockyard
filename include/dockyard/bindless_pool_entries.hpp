#pragma once

#include <dockyard/bindless_handle.hpp>
#include <dockyard/texture.hpp>

#include <volk.h>

namespace dy {

struct TextureEntry {
  Texture texture{};
  VkImageViewType sampled_view_type{VK_IMAGE_VIEW_TYPE_2D};
  auto operator==(const TextureEntry &) const -> bool = default;
};

struct SamplerEntry {
  VkSampler sampler = VK_NULL_HANDLE;
  auto operator==(const SamplerEntry &) const -> bool = default;
};

struct SubImageEntry {
  VkImageView view = VK_NULL_HANDLE;
  TextureHandle owner;
  u32 mip_level = 0U;
  u32 base_layer = 0U;
  u32 layer_count = 1U;
  auto operator==(const SubImageEntry &) const -> bool = default;
};

using TexturePool = Pool<TextureTag, TextureEntry>;
using SamplerPool = Pool<SamplerTag, SamplerEntry>;
using ComparisonSamplerPool = Pool<ComparisonSamplerTag, SamplerEntry>;
using SubImagePool = Pool<SubImageTag, SubImageEntry>;

template <> struct EntryDestroyer<TextureEntry> {
  static auto destroy(const VulkanContext &ctx, TextureEntry &e) -> void {
    e.texture.destroy(ctx);
  }
};

} // namespace dy
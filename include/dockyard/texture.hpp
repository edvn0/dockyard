#pragma once

#include "dockyard/vfs_path.hpp"
#include <dockyard/bindless_handle.hpp>
#include <dockyard/types.hpp>

#include <span>
#include <volk.h>

#include <vk_mem_alloc.h>

namespace dy {
template <typename Tag, typename Impl> class Pool;
struct SubImageTag;
struct SubImageEntry;
using SubImagePool = Pool<SubImageTag, SubImageEntry>;
} // namespace dy

namespace dy {

struct BindlessSet;

struct VulkanContext;

struct Texture {
  VkImage image{};
  VkImageView sampled_view{};
  VkImageView storage_view{};
  VmaAllocation allocation{};
  VmaAllocationInfo allocation_info{};
  VkFormat format{};
  VkExtent2D extent{};
  u32 mip_levels{1U};
  u32 array_layers{1U};
  bool owned{true};

  [[nodiscard]] auto valid() const -> bool { return image != VK_NULL_HANDLE; }
  constexpr auto operator==(const Texture &other) const -> bool {
    return image == other.image && sampled_view == other.sampled_view &&
           storage_view == other.storage_view && allocation == other.allocation;
  }

  struct CreateInfo {
    std::span<const std::byte> bytes{};
    u32 width{};
    u32 height{};
    VkFormat format{VK_FORMAT_R8G8B8A8_SRGB};
    bool generate_mips{false};
    bool storage_view{false};
  };

  static auto from_bytes(const VulkanContext &ctx, std::string_view name,
                         const CreateInfo &ci) -> Texture;
  static auto create(const VulkanContext &ctx, std::string_view name, u32 width,
                     u32 height, VkFormat format, VkImageUsageFlags usage,
                     VkImageAspectFlags aspect,
                     VkSampleCountFlagBits = VK_SAMPLE_COUNT_1_BIT) -> Texture;
  static auto load_hdr_texture(const VulkanContext &, const VFSPath &)
      -> Texture;

  struct CubemapInfo {
    u32 size;
    VkFormat format;
    u32 mip_levels;
    bool storage_view;
  };
  static auto create_cubemap(const VulkanContext &, std::string_view,
                             const CubemapInfo &) -> Texture;

  std::vector<VkImageView> sub_views;
  std::vector<SubImageHandle> sub_handles;

  [[nodiscard]] auto sub_view_handle(u32 mip, u32 view_within_mip = 0U) const
      -> SubImageHandle {
    const u32 views_per_mip = static_cast<u32>(sub_handles.size()) / mip_levels;
    assert(mip < mip_levels && view_within_mip < views_per_mip);
    return sub_handles[(mip * views_per_mip) + view_within_mip];
  }

  [[nodiscard]] auto has_sub_views() const -> bool {
    return !sub_handles.empty();
  }

  struct SubViewDesc {
    VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D;
    u32 layer_count = 1U;
  };
  auto register_sub_views(const VulkanContext &ctx, SubImagePool &sub_images,
                          BindlessSet &bindless,
                          SubViewDesc = {
                              .view_type = VK_IMAGE_VIEW_TYPE_2D,
                              .layer_count = 1U,
                          }) -> void;
  auto destroy(const VulkanContext &ctx, SubImagePool *sub_images = nullptr)
      -> void;
};

} // namespace dy

#pragma once

#include <dockyard/types.hpp>

#include <span>
#include <volk.h>

#include <vk_mem_alloc.h>

namespace dy {

struct VulkanContext;

struct Texture {
  VkImage image{};
  VkImageView sampled_view{};
  VkImageView storage_view{};
  VmaAllocation allocation{};
  VmaAllocationInfo allocation_info{};
  VkFormat format{};
  VkExtent2D extent{};
  u32 mip_levels{1};

  auto valid() const -> bool { return image != VK_NULL_HANDLE; }
  auto destroy(const VulkanContext &ctx) -> void;
  constexpr auto operator==(const Texture &other) const -> bool {
    return image == other.image && sampled_view == other.sampled_view &&
           storage_view == other.storage_view && allocation == other.allocation;
  }

  struct CreateInfo {
    std::span<const u32> bytes{}; // one f32 per pixel
    u32 width{};
    u32 height{};
    VkFormat format{VK_FORMAT_R8G8B8A8_SRGB};
    bool generate_mips{false};
    bool storage_view{false};
    VkQueue upload_queue{};
    u32 upload_queue_family{};
  };

  static auto from_bytes(const VulkanContext &ctx, std::string_view name,
                         const CreateInfo &ci) -> Texture;
  static auto create(const VulkanContext &ctx, std::string_view name, u32 width,
                     u32 height, VkFormat format, VkImageUsageFlags usage,
                     VkImageAspectFlags aspect,
                     VkSampleCountFlagBits = VK_SAMPLE_COUNT_1_BIT) -> Texture;
};

} // namespace dy

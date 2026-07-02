#include <dockyard/screenshot.hpp>

#include <dockyard/log.hpp>

#include <stb_image_write.h>

namespace dy {

namespace {
constexpr u32 screenshot_bytes_per_pixel = 4U;
} // namespace

auto record_screenshot_copy(const VulkanContext &ctx, VkCommandBuffer cmd,
                            const Texture &source,
                            std::filesystem::path output_path)
    -> std::expected<ScreenshotReadback, std::string> {
  if (source.format != VK_FORMAT_R8G8B8A8_SRGB &&
      source.format != VK_FORMAT_R8G8B8A8_UNORM) {
    return std::unexpected(std::format(
        "Screenshot capture only supports 8-bit RGBA formats, got VkFormat {}",
        static_cast<int>(source.format)));
  }

  const VkDeviceSize buffer_size = VkDeviceSize{source.extent.width} *
                                   source.extent.height *
                                   screenshot_bytes_per_pixel;

  auto staging = Buffer::create(ctx.allocator, "screenshot_readback",
                                buffer_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT);

  const VkImageSubresourceRange color_range{
      .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
      .baseMipLevel = 0U,
      .levelCount = 1U,
      .baseArrayLayer = 0U,
      .layerCount = 1U,
  };

  // The prior write to `source` is the composite pass's color attachment
  // output; make it visible to the transfer stage without touching layout
  // (VK_IMAGE_LAYOUT_GENERAL already permits use as a copy source).
  const VkImageMemoryBarrier2 to_transfer_read{
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
      .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
      .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
      .newLayout = VK_IMAGE_LAYOUT_GENERAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = source.image,
      .subresourceRange = color_range,
  };
  const VkDependencyInfo dep_info{
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = 1U,
      .pImageMemoryBarriers = &to_transfer_read,
  };
  vkCmdPipelineBarrier2(cmd, &dep_info);

  const VkBufferImageCopy2 region{
      .sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
      .bufferOffset = 0U,
      .bufferRowLength = 0U,
      .bufferImageHeight = 0U,
      .imageSubresource =
          {
              .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
              .mipLevel = 0U,
              .baseArrayLayer = 0U,
              .layerCount = 1U,
          },
      .imageOffset = {.x = 0, .y = 0, .z = 0},
      .imageExtent = {.width = source.extent.width,
                      .height = source.extent.height,
                      .depth = 1U,},
  };
  const VkCopyImageToBufferInfo2 copy_info{
      .sType = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2,
      .srcImage = source.image,
      .srcImageLayout = VK_IMAGE_LAYOUT_GENERAL,
      .dstBuffer = staging->get_buffer(),
      .regionCount = 1U,
      .pRegions = &region,
  };
  vkCmdCopyImageToBuffer2(cmd, &copy_info);

  return ScreenshotReadback{
      .staging = std::move(staging),
      .width = source.extent.width,
      .height = source.extent.height,
      .output_path = std::move(output_path),
  };
}

auto write_screenshot_png(const ScreenshotReadback &readback) -> void {
  readback.staging->invalidate();

  const auto stride =
      static_cast<int>(readback.width * screenshot_bytes_per_pixel);
  const auto wrote = stbi_write_png(
      readback.output_path.string().c_str(), static_cast<int>(readback.width),
      static_cast<int>(readback.height),
      static_cast<int>(screenshot_bytes_per_pixel),
      readback.staging->get_mapped_pointer(), stride);

  if (wrote == 0) {
    error("Failed to write screenshot to {}", readback.output_path.string());
    return;
  }
  info("Saved screenshot to {}", readback.output_path.string());
}

} // namespace dy

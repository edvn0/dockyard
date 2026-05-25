#include <dockyard/texture.hpp>

#include <bit>
#include <cassert>
#include <cstring>

#include <dockyard/app.hpp>
#include <dockyard/context.hpp>
#include <dockyard/vfs.hpp>

#include <stb_image.h>

namespace dy {
// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
namespace {

auto mip_count(u32 w, u32 h) -> u32 {
  return static_cast<u32>(std::bit_width(std::max(w, h)));
}

auto bytes_per_texel(VkFormat fmt) -> u32 {
  switch (fmt) {
  case VK_FORMAT_R8G8B8A8_SRGB:
  case VK_FORMAT_R8G8B8A8_UNORM:
  case VK_FORMAT_B8G8R8A8_SRGB:
  case VK_FORMAT_B8G8R8A8_UNORM:
    return 4;
  case VK_FORMAT_R8G8B8_SRGB:
  case VK_FORMAT_R8G8B8_UNORM:
    return 3;
  case VK_FORMAT_R8_UNORM:
    return 1;
  case VK_FORMAT_R16G16_UNORM:
  case VK_FORMAT_R16G16_SFLOAT:
    return 4;
  case VK_FORMAT_R16G16B16A16_SFLOAT:
    return 8;
  case VK_FORMAT_R32G32B32A32_SFLOAT:
    return 16;
  default:
    assert(false && "bytes_per_texel: unknown format");
    return 4;
  }
}

auto image_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout old_layout,
                   VkImageLayout new_layout, VkPipelineStageFlags2 src_stage,
                   VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                   VkAccessFlags2 dst_access,
                   VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                   u32 base_mip = 0, u32 mip_count = VK_REMAINING_MIP_LEVELS)
    -> void {
  const VkImageMemoryBarrier2 barrier{
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask = src_stage,
      .srcAccessMask = src_access,
      .dstStageMask = dst_stage,
      .dstAccessMask = dst_access,
      .oldLayout = old_layout,
      .newLayout = new_layout,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange =
          {
              .aspectMask = aspect,
              .baseMipLevel = base_mip,
              .levelCount = mip_count,
              .baseArrayLayer = 0,
              .layerCount = 1,
          },
  };
  const VkDependencyInfo dep{
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = 1,
      .pImageMemoryBarriers = &barrier,
  };
  vkCmdPipelineBarrier2(cmd, &dep);
}

auto blit_mip(VkCommandBuffer cmd, VkImage image, VkExtent2D src_extent,
              u32 src_mip) -> VkExtent2D {
  const VkExtent2D dst_extent{
      .width = std::max(1U, src_extent.width / 2),
      .height = std::max(1U, src_extent.height / 2),
  };

  image_barrier(
      cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
      VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
      VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT, src_mip, 1);

  const VkImageBlit2 region{
      .sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2,
      .srcSubresource =
          {
              .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
              .mipLevel = src_mip,
              .baseArrayLayer = 0,
              .layerCount = 1,
          },
      .srcOffsets =
          {
              VkOffset3D{
                  .x = 0,
                  .y = 0,
                  .z = 0,
              },
              VkOffset3D{
                  .x = static_cast<i32>(src_extent.width),
                  .y = static_cast<i32>(src_extent.height),
                  .z = 1,
              },
          },
      .dstSubresource =
          {
              .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
              .mipLevel = src_mip + 1,
              .baseArrayLayer = 0,
              .layerCount = 1,
          },
      .dstOffsets =
          {
              VkOffset3D{
                  .x = 0,
                  .y = 0,
                  .z = 0,
              },
              VkOffset3D{
                  .x = static_cast<i32>(dst_extent.width),
                  .y = static_cast<i32>(dst_extent.height),
                  .z = 1,
              },
          },
  };
  const VkBlitImageInfo2 blit{
      .sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2,
      .srcImage = image,
      .srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      .dstImage = image,
      .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .regionCount = 1,
      .pRegions = &region,
      .filter = VK_FILTER_LINEAR,
  };
  vkCmdBlitImage2(cmd, &blit);

  return dst_extent;
}

} // namespace

auto Texture::destroy(const VulkanContext &ctx, SubImagePool *sub_images)
    -> void {
  // Release pool slots immediately — CPU only, no GPU sync needed
  if (sub_images != nullptr) {
    for (auto h : sub_handles)
      sub_images->destroy(h);
  }
  sub_handles.clear();

  // GPU resources go through DeletionQueue as before
  auto image_ = this->image;
  auto sampled = this->sampled_view;
  auto storage = this->storage_view;
  auto alloc = this->allocation;
  auto &&mv = std::move(this->sub_views);

  DeletionQueue::the().push([o = this->owned, dev = ctx.device,
                             allocator = ctx.allocator, image_, sampled,
                             storage, alloc, mv = std::move(mv)]() mutable {
    for (auto &v : mv)
      vkDestroyImageView(dev, v, nullptr);

    if (o) {
      vkDestroyImageView(dev, sampled, nullptr);
      if (storage != sampled)
        vkDestroyImageView(dev, storage, nullptr);
      vmaDestroyImage(allocator, image_, alloc);
    }
  });

  *this = {};
}

auto Texture::create(const VulkanContext &ctx, std::string_view name, u32 width,
                     u32 height, VkFormat format, VkImageUsageFlags usage,
                     VkImageAspectFlags aspect, VkSampleCountFlagBits samples)
    -> Texture {
  Texture rt{};
  rt.format = format;
  rt.extent = {.width = width, .height = height};

  VkImageCreateInfo image_info{};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = format;
  image_info.extent = {.width = width, .height = height, .depth = 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = samples;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = usage;

  if (samples > VK_SAMPLE_COUNT_1_BIT) {
    image_info.usage |= VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
  }

  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  VmaAllocationCreateInfo vma_info{};
  vma_info.usage = VMA_MEMORY_USAGE_AUTO;

  if ((image_info.usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) != 0U) {
    vma_info.preferredFlags = VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT;
  }

  if (const auto result =
          vmaCreateImage(ctx.allocator, &image_info, &vma_info, &rt.image,
                         &rt.allocation, &rt.allocation_info);
      result != VK_SUCCESS) {
    error("Failed to create render target image: {}", result);
    std::abort();
  }
  vmaSetAllocationName(ctx.allocator, rt.allocation, name.data());
  VkDebugUtilsObjectNameInfoEXT name_info{};
  name_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
  name_info.pObjectName = name.data();
  name_info.objectType = VK_OBJECT_TYPE_IMAGE;
  name_info.objectHandle = std::bit_cast<u64>(rt.image);
  vkSetDebugUtilsObjectNameEXT(ctx.device, &name_info);

  VkImageViewCreateInfo view_info{};
  view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_info.image = rt.image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = format;
  view_info.subresourceRange.aspectMask = aspect;
  view_info.subresourceRange.baseMipLevel = 0;
  view_info.subresourceRange.levelCount = 1;
  view_info.subresourceRange.baseArrayLayer = 0;
  view_info.subresourceRange.layerCount = 1;

  if (const auto result =
          vkCreateImageView(ctx.device, &view_info, nullptr, &rt.sampled_view);
      result != VK_SUCCESS) {
    std::abort();
  }

  ctx.transition_to_general(rt.image, aspect, 1, 1);

  return rt;
}

auto Texture::load_hdr_texture(const VulkanContext &ctx, const VFSPath &path)
    -> Texture {
  int width{};
  int height{};
  int channels{};

  auto real_path = VFS::get().resolve(path);

  float *data =
      stbi_loadf(real_path.string().c_str(), &width, &height, &channels, 4);

  if (data == nullptr) {
    error("Failed to load HDR: {}", stbi_failure_reason());
    std::abort();
  }

  const auto byte_size = static_cast<usize>(width) * height * 4 * sizeof(u32);
  const std::span<const u32> bytes{reinterpret_cast<const u32 *>(data),
                                   byte_size};

  auto tex = Texture::from_bytes(ctx, real_path.filename().string(),
                                 {
                                     .bytes = bytes,
                                     .width = static_cast<u32>(width),
                                     .height = static_cast<u32>(height),
                                     .format = VK_FORMAT_R32G32B32A32_SFLOAT,
                                     .generate_mips = false,
                                     .storage_view = false,
                                 });

  stbi_image_free(data);
  return tex;
}

auto Texture::from_bytes(const VulkanContext &ctx, std::string_view name,
                         const CreateInfo &ci) -> Texture {
  assert(!ci.bytes.empty() && ci.width > 0 && ci.height > 0);

  const u32 mips = ci.generate_mips ? mip_count(ci.width, ci.height) : 1u;
  const VkDeviceSize byte_size = static_cast<VkDeviceSize>(ci.width) *
                                 ci.height * bytes_per_texel(ci.format);

  const VmaAllocationCreateInfo staging_alloc_ci{
      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
               VMA_ALLOCATION_CREATE_MAPPED_BIT,
      .usage = VMA_MEMORY_USAGE_AUTO,
  };
  const VkBufferCreateInfo staging_buf_ci{
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = byte_size,
      .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
  };
  VkBuffer staging_buf{};
  VmaAllocation staging_alloc{};
  VmaAllocationInfo staging_info{};
  vmaCreateBuffer(ctx.allocator, &staging_buf_ci, &staging_alloc_ci,
                  &staging_buf, &staging_alloc, &staging_info);
  std::memcpy(staging_info.pMappedData, ci.bytes.data(), byte_size);
  vmaFlushAllocation(ctx.allocator, staging_alloc, 0, VK_WHOLE_SIZE);

  VkImageUsageFlags usage =
      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  if (ci.generate_mips)
    usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  if (ci.storage_view)
    usage |= VK_IMAGE_USAGE_STORAGE_BIT;

  const VkImageCreateInfo image_ci{
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = ci.format,
      .extent = {.width = ci.width, .height = ci.height, .depth = 1},
      .mipLevels = mips,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
  };
  const VmaAllocationCreateInfo image_alloc_ci{
      .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
  };

  Texture tex{};
  tex.format = ci.format;
  tex.extent = {.width = ci.width, .height = ci.height};
  tex.mip_levels = mips;

  vk::check(vmaCreateImage(ctx.allocator, &image_ci, &image_alloc_ci,
                           &tex.image, &tex.allocation, &tex.allocation_info));
  vmaSetAllocationName(ctx.allocator, tex.allocation, name.data());
  VkDebugUtilsObjectNameInfoEXT name_info{};
  name_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
  name_info.pObjectName = name.data();
  name_info.objectType = VK_OBJECT_TYPE_IMAGE;
  name_info.objectHandle = std::bit_cast<u64>(tex.image);
  vkSetDebugUtilsObjectNameEXT(ctx.device, &name_info);

  ctx.one_time_submit([&t = tex, &ci, &staging_buf, mips](const auto &cmd) {
    // Whole image: UNDEFINED -> TRANSFER_DST
    image_barrier(cmd, t.image, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                  VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                  VK_ACCESS_2_TRANSFER_WRITE_BIT);

    // Copy buffer -> mip 0
    const VkBufferImageCopy2 copy{
        .sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        .imageOffset = {.x = 0, .y = 0, .z = 0},
        .imageExtent = {.width = ci.width, .height = ci.height, .depth = 1},
    };
    const VkCopyBufferToImageInfo2 copy_info{
        .sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2,
        .srcBuffer = staging_buf,
        .dstImage = t.image,
        .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .regionCount = 1,
        .pRegions = &copy,
    };
    vkCmdCopyBufferToImage2(cmd, &copy_info);

    if (ci.generate_mips && mips > 1) {
      VkExtent2D src_extent{.width = ci.width, .height = ci.height};
      for (u32 m = 0; m < mips - 1; ++m) {
        src_extent = blit_mip(cmd, t.image, src_extent, m);
      }
      // Transition all src mips (0..mips-2: TRANSFER_SRC) + last mip
      // (TRANSFER_DST)
      // -> SHADER_READ_ONLY in one barrier each group.
      image_barrier(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 0,
                    mips - 1);

      image_barrier(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                    mips - 1, 1);
    } else {
      // No mips: single barrier for mip 0
      image_barrier(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_READ_BIT);
    }
  });

  vmaDestroyBuffer(ctx.allocator, staging_buf, staging_alloc);

  // ------------------------------------------------------------------
  // 4. Image views
  // ------------------------------------------------------------------
  const VkImageViewCreateInfo view_ci{
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = tex.image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = ci.format,
      .subresourceRange =
          {
              .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
              .baseMipLevel = 0,
              .levelCount = mips,
              .baseArrayLayer = 0,
              .layerCount = 1,
          },
  };
  vkCreateImageView(ctx.device, &view_ci, nullptr, &tex.sampled_view);

  if (ci.storage_view) {
    // Storage views must address a single mip level (base 0).
    auto storage_view_ci = view_ci;
    storage_view_ci.subresourceRange.levelCount = 1;
    vkCreateImageView(ctx.device, &storage_view_ci, nullptr, &tex.storage_view);
  }

  return tex;
}

auto Texture::create_cubemap(const VulkanContext &ctx, std::string_view name,
                             const CubemapInfo &ci) -> Texture {
  Texture tex{};
  tex.format = ci.format;
  tex.extent = {.width = ci.size, .height = ci.size};
  tex.mip_levels = ci.mip_levels;
  tex.array_layers = 6U;

  const VkImageCreateInfo image_ci{
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = ci.format,
      .extent =
          {
              .width = ci.size,
              .height = ci.size,
              .depth = 1,
          },
      .mipLevels = ci.mip_levels,
      .arrayLayers = 6,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
               (ci.mip_levels > 1 ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : 0u),
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
  };
  const VmaAllocationCreateInfo alloc_ci{
      .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};

  vk::check(vmaCreateImage(ctx.allocator, &image_ci, &alloc_ci, &tex.image,
                           &tex.allocation, &tex.allocation_info));
  vmaSetAllocationName(ctx.allocator, tex.allocation, name.data());
  VkDebugUtilsObjectNameInfoEXT name_info{};
  name_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
  name_info.pObjectName = name.data();
  name_info.objectType = VK_OBJECT_TYPE_IMAGE;
  name_info.objectHandle = std::bit_cast<u64>(tex.image);
  vkSetDebugUtilsObjectNameEXT(ctx.device, &name_info);

  const VkImageViewCreateInfo view_ci{
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = tex.image,
      .viewType = VK_IMAGE_VIEW_TYPE_CUBE, // ← key
      .format = ci.format,
      .subresourceRange =
          {
              .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
              .baseMipLevel = 0,
              .levelCount = ci.mip_levels,
              .baseArrayLayer = 0,
              .layerCount = 6,
          },
  };
  vkCreateImageView(ctx.device, &view_ci, nullptr, &tex.sampled_view);

  if (ci.storage_view) {
    auto s_ci = view_ci;
    s_ci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    s_ci.subresourceRange.levelCount = 1;
    s_ci.subresourceRange.layerCount = 6;
    vkCreateImageView(ctx.device, &s_ci, nullptr, &tex.storage_view);
  }

  ctx.one_time_submit([&tex, &ci](VkCommandBuffer cmd) {
    const VkImageMemoryBarrier2 b{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .image = tex.image,
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = ci.mip_levels,
                .baseArrayLayer = 0,
                .layerCount = 6,
            },
    };
    const VkDependencyInfo dep{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &b,
    };
    vkCmdPipelineBarrier2(cmd, &dep);
  });

  return tex;
}

auto Texture::register_sub_views(const VulkanContext &ctx,
                                 SubImagePool &sub_images,
                                 BindlessSet &bindless, SubViewDesc desc)
    -> void {
  assert(sub_views.empty());
  assert((array_layers % desc.layer_count) == 0u &&
         "layer_count must evenly divide array_layers");

  const u32 views_per_mip = array_layers / desc.layer_count;
  const u32 total = mip_levels * views_per_mip;
  sub_views.resize(total);
  sub_handles.resize(total);

  for (u32 mip = 0u; mip < mip_levels; ++mip) {
    for (u32 v = 0u; v < views_per_mip; ++v) {
      const u32 idx = (mip * views_per_mip) + v;
      const u32 base_layer = v * desc.layer_count;

      const VkImageViewCreateInfo view_ci{
          .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
          .image = image,
          .viewType = desc.view_type,
          .format = format,
          .subresourceRange =
              {
                  .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                  .baseMipLevel = mip,
                  .levelCount = 1U,
                  .baseArrayLayer = base_layer,
                  .layerCount = desc.layer_count,
              },
      };
      vk::check(
          vkCreateImageView(ctx.device, &view_ci, nullptr, &sub_views[idx]));

      const auto h = sub_images.create(SubImageEntry{
          .view = sub_views[idx],
          .mip_level = mip,
          .base_layer = base_layer,
          .layer_count = desc.layer_count,
      });

      sub_handles[idx] = h;
      bindless.mark_dirty();
    }
  }
}

} // namespace dy
#include <dockyard/texture.hpp>

#include <bit>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <string>

#include <dockyard/app.hpp>
#include <dockyard/context.hpp>
#include <dockyard/crash_reporter.hpp>
#include <dockyard/texture_upload_pool.hpp>
#include <dockyard/vfs.hpp>

#include <volk.h>

#include <ktx.h>
#include <ktxvulkan.h>

#include <glm/gtc/packing.hpp>
#include <stb_image.h>
#include <stb_image_write.h>

namespace dy {

namespace {

struct KtxHdrMip {
  std::vector<std::byte> data;
  u32 width{};
  u32 height{};
};

struct KtxHdrDecodeResult {
  std::vector<KtxHdrMip> mips;
  VkFormat vk_format{VK_FORMAT_UNDEFINED};
  u32 width{};
  u32 height{};
};

struct KtxTexture2Guard {
  ktxTexture2 *texture{};

  ~KtxTexture2Guard() {
    if (texture != nullptr) {
      ktxTexture2_Destroy(texture);
    }
  }

  explicit KtxTexture2Guard(ktxTexture2 *tex) : texture(tex) {}
  KtxTexture2Guard() = default;

  KtxTexture2Guard(KtxTexture2Guard &&other) noexcept
      : texture(other.release()) {}

  KtxTexture2Guard &operator=(KtxTexture2Guard &&other) noexcept {
    if (this != &other) {
      if (texture != nullptr) {
        ktxTexture2_Destroy(texture);
      }
      texture = other.release();
    }
    return *this;
  }

  KtxTexture2Guard &operator=(const KtxTexture2Guard &) = delete;

  KtxTexture2Guard(const KtxTexture2Guard &) = delete;

  [[nodiscard]] ktxTexture2 *release() noexcept {
    ktxTexture2 *result = texture;
    texture = nullptr;
    return result;
  }
};

[[nodiscard]] auto ktx_error_string(KTX_error_code error) -> std::string {
  return std::format("{}", ktxErrorString(error));
}

[[nodiscard]] auto is_sampled_format_supported(const VulkanContext &ctx,
                                               VkFormat format) -> bool {
  VkFormatProperties props{};
  vkGetPhysicalDeviceFormatProperties(ctx.physical_device, format, &props);
  return (props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) !=
         0;
}

[[nodiscard]] auto transcode_ktx2_hdr_if_needed(const VulkanContext &ctx,
                                                ktxTexture2 *ktx)
    -> std::expected<VkFormat, std::string> {
  constexpr VkFormat uastc_hdr_intermediate = VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK;

  const bool needs_transcode =
      ktxTexture2_NeedsTranscoding(ktx) ||
      static_cast<VkFormat>(ktx->vkFormat) == uastc_hdr_intermediate;

  if (needs_transcode) {
    constexpr VkFormat target_fmt = VK_FORMAT_BC6H_UFLOAT_BLOCK;
    if (!is_sampled_format_supported(ctx, target_fmt)) {
      return std::unexpected(
          "VK_FORMAT_BC6H_UFLOAT_BLOCK not supported on this device");
    }
    const auto err = ktxTexture2_TranscodeBasis(ktx, KTX_TTF_BC6HU, 0);
    if (err != KTX_SUCCESS) {
      return std::unexpected(std::format(
          "ktxTexture2_TranscodeBasis failed: {}", ktx_error_string(err)));
    }
    return target_fmt;
  }

  // Already a native GPU format — validate it.
  const auto fmt = static_cast<VkFormat>(ktx->vkFormat);
  if (fmt == VK_FORMAT_UNDEFINED) {
    return std::unexpected(
        "KTX2 has VK_FORMAT_UNDEFINED with no transcoding needed");
  }
  if (!is_sampled_format_supported(ctx, fmt)) {
    return std::unexpected(std::format(
        "KTX2 VkFormat {} not supported as sampled optimal-tiled image",
        std::to_underlying(fmt)));
  }
  return fmt;
}

[[nodiscard]] auto decode_ktx2_hdr_texture(const VulkanContext &ctx,
                                           const std::filesystem::path &path)
    -> std::expected<KtxHdrDecodeResult, std::string> {
  ktxTexture2 *raw_ktx{};

  const auto create_error = ktxTexture2_CreateFromNamedFile(
      path.string().c_str(), KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &raw_ktx);

  if (create_error != KTX_SUCCESS) {
    return std::unexpected(
        std::format("ktxTexture2_CreateFromNamedFile failed: {}",
                    ktx_error_string(create_error)));
  }

  KtxTexture2Guard guard{raw_ktx};
  auto *ktx = guard.texture;

  if (ktx->numDimensions != 2) {
    return std::unexpected(
        std::format("Expected 2D KTX2 HDR equirect texture, got {} dimensions",
                    ktx->numDimensions));
  }

  if (ktx->numFaces != 1) {
    return std::unexpected(
        std::format("Expected equirect KTX2 texture with 1 face, got {} faces",
                    ktx->numFaces));
  }

  if (ktx->numLayers > 1) {
    return std::unexpected(std::format(
        "Expected non-array KTX2 texture, got {} layers", ktx->numLayers));
  }

  auto format = transcode_ktx2_hdr_if_needed(ctx, ktx);
  if (!format) {
    return std::unexpected(format.error());
  }

  KtxHdrDecodeResult out;
  out.width = ktx->baseWidth;
  out.height = ktx->baseHeight;
  out.vk_format = *format;

  const auto mip_count = std::max(1u, ktx->numLevels);
  out.mips.reserve(mip_count);

  for (u32 level = 0; level < mip_count; ++level) {
    ktx_size_t offset{};

    const auto offset_error =
        ktxTexture_GetImageOffset(ktxTexture(ktx), level, 0, 0, &offset);

    if (offset_error != KTX_SUCCESS) {
      return std::unexpected(
          std::format("ktxTexture_GetImageOffset failed for mip {}: {}", level,
                      ktx_error_string(offset_error)));
    }

    const auto size = ktxTexture_GetImageSize(ktxTexture(ktx), level);
    const auto *src = reinterpret_cast<const std::byte *>(ktx->pData + offset);

    KtxHdrMip mip;
    mip.width = std::max(1u, ktx->baseWidth >> level);
    mip.height = std::max(1u, ktx->baseHeight >> level);
    mip.data.assign(src, src + size);

    out.mips.push_back(std::move(mip));
  }

  return out;
}

} // namespace

namespace {

auto mip_count(u32 w, u32 h) -> u32 {
  return static_cast<u32>(std::bit_width(std::max(w, h)));
}

struct FormatBlockInfo {
  u32 block_width;
  u32 block_height;
  u32 block_size_bytes;
};

auto get_format_block_info(VkFormat format) -> FormatBlockInfo {
  switch (format) {
  case VK_FORMAT_ASTC_4x4_UNORM_BLOCK:
    return {4, 4, 16};
  case VK_FORMAT_ASTC_5x4_UNORM_BLOCK:
    return {5, 4, 16};
  case VK_FORMAT_ASTC_5x5_UNORM_BLOCK:
    return {5, 5, 16};
  // Add other ASTC HDR/LDR variants if needed...

  // BC6H (The format you transcode to in your fallback code)
  case VK_FORMAT_BC6H_UFLOAT_BLOCK:
  case VK_FORMAT_BC6H_SFLOAT_BLOCK:
    return {4, 4, 16};

  // BC1/BC4 — 4x4 blocks, 8 bytes (half-rate: 1-bit alpha or single channel).
  case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
  case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
  case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
  case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
  case VK_FORMAT_BC4_UNORM_BLOCK:
  case VK_FORMAT_BC4_SNORM_BLOCK:
    return {4, 4, 8};

  // BC2/BC3/BC5/BC7 — 4x4 blocks, 16 bytes. BC7 is the default colour-texture
  // transcode target, BC5 the normal-map target (see ktx_transcode_format in
  // mesh_loader.cpp) — both previously fell through to the 1x1/4-byte
  // default below, undersizing the staging buffer for any texture whose
  // dimensions weren't large enough to mask the miscalculation.
  case VK_FORMAT_BC2_UNORM_BLOCK:
  case VK_FORMAT_BC2_SRGB_BLOCK:
  case VK_FORMAT_BC3_UNORM_BLOCK:
  case VK_FORMAT_BC3_SRGB_BLOCK:
  case VK_FORMAT_BC5_UNORM_BLOCK:
  case VK_FORMAT_BC5_SNORM_BLOCK:
  case VK_FORMAT_BC7_UNORM_BLOCK:
  case VK_FORMAT_BC7_SRGB_BLOCK:
    return {4, 4, 16};

  // Standard uncompressed formats fall back to 1x1 blocks
  case VK_FORMAT_R8G8B8A8_UNORM:
    return {1, 1, 4};
  case VK_FORMAT_R16G16B16A16_SFLOAT:
    return {1, 1, 8};
  case VK_FORMAT_R32G32B32A32_SFLOAT:
    return {1, 1, 16};

  default:
    return {.block_width = 1, .block_height = 1, .block_size_bytes = 4};
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
              VkOffset3D{.x = 0, .y = 0, .z = 0},
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
              VkOffset3D{.x = 0, .y = 0, .z = 0},
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

auto set_view_debug_name(VkDevice device, VkImageView view,
                         const std::string_view name) -> void {
  VkDebugUtilsObjectNameInfoEXT name_info{};
  name_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
  name_info.objectType = VK_OBJECT_TYPE_IMAGE_VIEW;
  name_info.objectHandle = std::bit_cast<u64>(view);
  name_info.pObjectName = name.data();
  vkSetDebugUtilsObjectNameEXT(device, &name_info);
}

} // namespace

auto Texture::load_ktx2_hdr_texture(const VulkanContext &ctx,
                                    const VFSPath &path)
    -> std::expected<Texture, std::string> {
  const auto real_path = VFS::get().resolve(path);

  auto result = decode_ktx2_hdr_texture(ctx, real_path);
  if (!result) {
    const auto str = std::format("Failed to decode KTX2 HDR texture '{}': {}",
                                 real_path.string(), result.error());
    error("{}", str);
    return std::unexpected(str);
  }

  auto &decoded = *result;
  assert(!decoded.mips.empty());

  // Base level is mips[0]; the remainder feed CreateInfo::mips.
  const std::span<const std::byte> base_bytes = decoded.mips[0].data;

  std::vector<pool::MipData> extra_mips;
  extra_mips.reserve(decoded.mips.size() - 1);
  for (size_t i = 1; i < decoded.mips.size(); ++i) {
    extra_mips.push_back({
        .pixels = decoded.mips[i].data,
        .width = decoded.mips[i].width,
        .height = decoded.mips[i].height,
    });
  }

  return Texture::from_bytes(ctx, real_path.filename().string(),
                             {
                                 .bytes = base_bytes,
                                 .mips = extra_mips,
                                 .width = decoded.width,
                                 .height = decoded.height,
                                 .format = decoded.vk_format,
                                 .generate_mips = false,
                                 .storage_view = false,
                             });
}

auto Texture::destroy(const VulkanContext &ctx, SubImagePool *sub_images)
    -> void {
  if (sub_images != nullptr) {
    for (auto h : sub_handles)
      sub_images->destroy(h);
  }
  sub_handles.clear();

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

auto Texture::destroy(const VulkanContext &ctx,
                      TexturePool *mip_layer_image_pool) -> void {
  if (mip_layer_image_pool != nullptr) {
    for (auto h : mip_layer_handles)
      mip_layer_image_pool->destroy(h);
  }
  mip_layer_handles.clear();

  if (!mip_layer_views.empty()) {
    DeletionQueue::the().push(
        [dev = ctx.device, views = std::move(mip_layer_views)]() {
          for (auto v : views)
            if (v != VK_NULL_HANDLE)
              vkDestroyImageView(dev, v, nullptr);
        });
  }

  destroy(ctx);
}

void Texture::destroy(const VulkanContext &ctx) {
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
                     VkImageAspectFlags aspect, VkSampleCountFlagBits samples,
                     u32 mips, bool dedicated_memory) -> Texture {
  breadcrumb("texture_create");
  Texture rt{};
  rt.format = format;
  rt.extent = {.width = width, .height = height};
  rt.mip_levels = mips;
  rt.name = name;

  VkImageCreateInfo image_info{};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = format;
  image_info.extent = {.width = width, .height = height, .depth = 1};
  image_info.mipLevels = mips;
  image_info.arrayLayers = 1;
  image_info.samples = samples;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = usage;

  const bool transient =
      samples > VK_SAMPLE_COUNT_1_BIT && ctx.caps.transient_attachments;
  if (transient) {
    image_info.usage |= VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
  }

  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  VmaAllocationCreateInfo vma_info{};
  vma_info.usage =
      transient ? VMA_MEMORY_USAGE_GPU_LAZILY_ALLOCATED : VMA_MEMORY_USAGE_AUTO;

  if (dedicated_memory) {
    vma_info.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
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
  view_info.subresourceRange.levelCount = mips;
  view_info.subresourceRange.baseArrayLayer = 0;
  view_info.subresourceRange.layerCount = 1;

  if (const auto result =
          vkCreateImageView(ctx.device, &view_info, nullptr, &rt.sampled_view);
      result != VK_SUCCESS) {
    std::abort();
  }
  set_view_debug_name(ctx.device, rt.sampled_view,
                      std::string(name) + "_sampled_view");

  if ((usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0U) {
    auto storage_view_info = view_info;
    storage_view_info.subresourceRange.levelCount = 1;
    if (const auto result = vkCreateImageView(ctx.device, &storage_view_info,
                                              nullptr, &rt.storage_view);
        result != VK_SUCCESS) {
      std::abort();
    }
    set_view_debug_name(ctx.device, rt.storage_view,
                        std::string(name) + "_storage_view");
  }

  // A combined depth-stencil image must always be transitioned with both
  // aspects present in the barrier (VUID-VkImageMemoryBarrier2-image-03320),
  // even when the caller only ever samples/attaches the depth aspect (e.g.
  // depth_resolved_target, whose format is forced to match depth_msaa's).
  VkImageAspectFlags barrier_aspect = aspect;
  if (format == VK_FORMAT_D16_UNORM_S8_UINT ||
      format == VK_FORMAT_D24_UNORM_S8_UINT ||
      format == VK_FORMAT_D32_SFLOAT_S8_UINT) {
    barrier_aspect |= VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
  }
  ctx.transition_to_general(rt.image, barrier_aspect, mips, 1);

  return rt;
}

auto Texture::from_bytes(const VulkanContext &ctx, std::string_view name,
                         const CreateInfo &ci) -> Texture {
  breadcrumb("texture_from_bytes");
  assert(ci.width > 0 && ci.height > 0);

  const bool has_custom_mips = !ci.mips.empty();

  u32 mips = 1u;
  VkDeviceSize total_byte_size = 0;

  struct MipCopyRegion {
    VkDeviceSize buffer_offset;
    u32 width;
    u32 height;
  };
  std::vector<MipCopyRegion> copy_regions;

  const auto block_info = get_format_block_info(ci.format);

  if (has_custom_mips) {
    mips = static_cast<u32>(ci.mips.size() + 1);
    copy_regions.reserve(mips);

    total_byte_size = static_cast<VkDeviceSize>(ci.bytes.size_bytes());
    copy_regions.push_back({
        .buffer_offset = 0,
        .width = ci.width,
        .height = ci.height,
    });

    VkDeviceSize current_offset = total_byte_size;
    for (const auto &mip : ci.mips) {
      copy_regions.push_back({current_offset, mip.width, mip.height});
      current_offset += static_cast<VkDeviceSize>(mip.pixels.size());
    }
    total_byte_size = current_offset;
  } else {
    mips = ci.generate_mips ? mip_count(ci.width, ci.height) : 1u;

    // Round up dimensions to block boundaries
    u32 num_blocks_x =
        (ci.width + block_info.block_width - 1) / block_info.block_width;
    u32 num_blocks_y =
        (ci.height + block_info.block_height - 1) / block_info.block_height;

    total_byte_size = static_cast<VkDeviceSize>(num_blocks_x) * num_blocks_y *
                      block_info.block_size_bytes;
    copy_regions.push_back({0, ci.width, ci.height});
  }

  const VmaAllocationCreateInfo staging_alloc_ci{
      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
               VMA_ALLOCATION_CREATE_MAPPED_BIT,
      .usage = VMA_MEMORY_USAGE_AUTO,
  };
  const VkBufferCreateInfo staging_buf_ci{
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = total_byte_size,
      .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
  };
  VkBuffer staging_buf{};
  VmaAllocation staging_alloc{};
  VmaAllocationInfo staging_info{};
  vmaCreateBuffer(ctx.allocator, &staging_buf_ci, &staging_alloc_ci,
                  &staging_buf, &staging_alloc, &staging_info);
  {
    auto staging_name = std::format("{}_staging_buffer", name);
    VkDebugUtilsObjectNameInfoEXT name_info{
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
        .objectType = VK_OBJECT_TYPE_BUFFER,
        .objectHandle = std::bit_cast<u64>(staging_buf),
        .pObjectName = staging_name.c_str(),
    };
    vkSetDebugUtilsObjectNameEXT(ctx.device, &name_info);
  }

  auto *dst_ptr = static_cast<u8 *>(staging_info.pMappedData);
  if (has_custom_mips) {
    std::memcpy(dst_ptr + copy_regions[0].buffer_offset, ci.bytes.data(),
                ci.bytes.size_bytes());
    for (size_t i = 0; i < ci.mips.size(); ++i) {
      std::memcpy(dst_ptr + copy_regions[i + 1].buffer_offset,
                  ci.mips[i].pixels.data(), ci.mips[i].pixels.size());
    }
  } else {
    std::memcpy(dst_ptr, ci.bytes.data(), total_byte_size);
  }
  vmaFlushAllocation(ctx.allocator, staging_alloc, 0, VK_WHOLE_SIZE);

  VkImageUsageFlags usage =
      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  if (ci.generate_mips && !has_custom_mips)
    usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  if (ci.storage_view)
    usage |= VK_IMAGE_USAGE_STORAGE_BIT;
  usage |= ci.extra_usage;

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
      .flags =
          ci.dedicated_memory ? VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT : 0u,
      .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
  };

  Texture tex{};
  tex.format = ci.format;
  tex.extent = {.width = ci.width, .height = ci.height};
  tex.mip_levels = mips;
  tex.name = name;

  vk::check(vmaCreateImage(ctx.allocator, &image_ci, &image_alloc_ci,
                           &tex.image, &tex.allocation, &tex.allocation_info));
  vmaSetAllocationName(ctx.allocator, tex.allocation, tex.name.c_str());

  {
    const VkDebugUtilsObjectNameInfoEXT name_info{
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
        .objectType = VK_OBJECT_TYPE_IMAGE,
        .objectHandle = std::bit_cast<u64>(tex.image),
        .pObjectName = tex.name.c_str(),
    };
    vkSetDebugUtilsObjectNameEXT(ctx.device, &name_info);
  }

  ctx.one_time_submit([&t = tex, &staging_buf, &copy_regions, mips,
                       has_custom_mips, &ci](const auto &cmd) {
    image_barrier(
        cmd, t.image, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE,
        VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 0, mips);

    std::vector<VkBufferImageCopy2> copies;
    copies.reserve(copy_regions.size());

    for (u32 m = 0; m < copy_regions.size(); ++m) {
      copies.push_back({
          .sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
          .bufferOffset = copy_regions[m].buffer_offset,
          .bufferRowLength = 0,
          .bufferImageHeight = 0,
          .imageSubresource =
              {
                  .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                  .mipLevel = m,
                  .baseArrayLayer = 0,
                  .layerCount = 1,
              },
          .imageOffset = {.x = 0, .y = 0, .z = 0},
          .imageExtent = {.width = copy_regions[m].width,
                          .height = copy_regions[m].height,
                          .depth = 1},
      });
    }

    const VkCopyBufferToImageInfo2 copy_info{
        .sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2,
        .srcBuffer = staging_buf,
        .dstImage = t.image,
        .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .regionCount = static_cast<u32>(copies.size()),
        .pRegions = copies.data(),
    };
    vkCmdCopyBufferToImage2(cmd, &copy_info);

    if (has_custom_mips) {
      image_barrier(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 0,
                    mips);
    } else if (ci.generate_mips && mips > 1) {
      VkExtent2D src_extent{.width = ci.width, .height = ci.height};
      for (u32 m = 0; m < mips - 1; ++m) {
        src_extent = blit_mip(cmd, t.image, src_extent, m);
      }
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
      image_barrier(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_READ_BIT);
    }
  });

  vmaDestroyBuffer(ctx.allocator, staging_buf, staging_alloc);

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
  set_view_debug_name(ctx.device, tex.sampled_view,
                      std::format("{}_sampled_view", tex.name));

  if (ci.storage_view) {
    auto storage_view_ci = view_ci;
    storage_view_ci.subresourceRange.levelCount = 1;
    vkCreateImageView(ctx.device, &storage_view_ci, nullptr, &tex.storage_view);
    set_view_debug_name(ctx.device, tex.storage_view,
                        std::format("{}_storage_view", tex.name));
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
  tex.name = name;

  const VkImageCreateInfo image_ci{
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = ci.format,
      .extent = {.width = ci.size, .height = ci.size, .depth = 1},
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
      .flags =
          ci.dedicated_memory ? VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT : 0u,
      .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
  };

  vk::check(vmaCreateImage(ctx.allocator, &image_ci, &alloc_ci, &tex.image,
                           &tex.allocation, &tex.allocation_info));
  vmaSetAllocationName(ctx.allocator, tex.allocation, tex.name.c_str());

  VkDebugUtilsObjectNameInfoEXT name_info{};
  name_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
  name_info.pObjectName = name.data();
  name_info.objectType = VK_OBJECT_TYPE_IMAGE;
  name_info.objectHandle = std::bit_cast<u64>(tex.image);
  vkSetDebugUtilsObjectNameEXT(ctx.device, &name_info);

  const VkImageViewCreateInfo view_ci{
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = tex.image,
      .viewType = VK_IMAGE_VIEW_TYPE_CUBE,
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
  set_view_debug_name(ctx.device, tex.sampled_view,
                      std::format("{}_sampled_cube_view", tex.name));

  if (ci.storage_view) {
    auto s_ci = view_ci;
    s_ci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    s_ci.subresourceRange.levelCount = 1;
    s_ci.subresourceRange.layerCount = 6;
    vkCreateImageView(ctx.device, &s_ci, nullptr, &tex.storage_view);
    set_view_debug_name(ctx.device, tex.storage_view,
                        std::format("{}_storage_array_view", tex.name));
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

      set_view_debug_name(
          ctx.device, sub_views[idx],
          std::format("subimage_pool_{}_view_m{}_l{}", name, mip, base_layer));

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

auto Texture::register_sub_views(const VulkanContext &ctx,
                                 TexturePool &texture_pool,
                                 BindlessSet &bindless, SubViewDesc desc)
    -> void {
  assert(mip_layer_handles.empty());
  assert((array_layers % desc.layer_count) == 0u &&
         "layer_count must evenly divide array_layers");

  const u32 views_per_mip = array_layers / desc.layer_count;
  const u32 total = mip_levels * views_per_mip;
  mip_layer_handles.resize(total);

  const VkImageAspectFlags format_aspect =
      (format == VK_FORMAT_D32_SFLOAT || format == VK_FORMAT_D16_UNORM)
          ? VK_IMAGE_ASPECT_DEPTH_BIT
          : VK_IMAGE_ASPECT_COLOR_BIT;

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
                  .aspectMask = format_aspect,
                  .baseMipLevel = mip,
                  .levelCount = 1U,
                  .baseArrayLayer = base_layer,
                  .layerCount = desc.layer_count,
              },
      };

      VkImageView sub_view = VK_NULL_HANDLE;
      vk::check(vkCreateImageView(ctx.device, &view_ci, nullptr, &sub_view));

      set_view_debug_name(ctx.device, sub_view,
                          std::format("texture_pool_{}_mip={}_layer={}", name,
                                      mip, base_layer));

      mip_layer_views.push_back(sub_view);

      Texture sub_tex{
          .image = image,
          .sampled_view = sub_view,
          .storage_view = sub_view,
          .format = format,
          .extent = {.width = std::max(1U, extent.width >> mip),
                     .height = std::max(1U, extent.height >> mip)},
          .mip_levels = 1U,
          .array_layers = desc.layer_count,
          .owned = false,
      };

      const auto h = texture_pool.create(TextureEntry{
          .texture = std::move(sub_tex),
          .sampled_view_type = desc.view_type,
      });

      mip_layer_handles[idx] = h;
    }
  }
  bindless.mark_dirty();
}

namespace {
// Assuming a standard RGBA 32-bit float source image structure
struct FloatPixel {
  float r, g, b, a;
};

struct HDRScaleResult {
  float scale_factor;
  float shader_multiplier;
};

auto calculate_hdr_scale(std::span<const FloatPixel> &source_pixels)
    -> HDRScaleResult {
  float max_value = 0.0F;

  // 1. Find the maximum finite value in the source image
  for (const auto &pixel : source_pixels) {
    // We generally only care about color channels (RGB) for HDR scaling
    for (float channel : {pixel.r, pixel.g, pixel.b}) {
      if (std::isfinite(channel)) {
        max_value = std::max(max_value, std::abs(channel));
      }
    }
  }

  if (max_value <= 0.0F) {
    return {.scale_factor = 1.0F, .shader_multiplier = 1.0};
  }

  constexpr auto astc_hdr_max = 65216.0F;

  if (max_value > astc_hdr_max) {
    float compression_scale = astc_hdr_max / max_value;

    float shader_multiplier = max_value / astc_hdr_max;

    return {.scale_factor = compression_scale,
            .shader_multiplier = shader_multiplier};
  }

  return {.scale_factor = 1.0F, .shader_multiplier = 1.0F};
}

[[nodiscard]] auto convert_hdr_to_ktx2(const std::filesystem::path &input,
                                       const std::filesystem::path &output)
    -> std::expected<void, std::string> {
  int width{};
  int height{};
  int channels{};
  float *data =
      stbi_loadf(input.string().c_str(), &width, &height, &channels, 4);
  if (data == nullptr) {
    return std::unexpected(
        std::format("stbi_loadf failed: {}", stbi_failure_reason()));
  }

  for (usize px = 0; px < static_cast<usize>(width) * height; ++px) {
    float *p = data + (px * 4);
    for (int c = 0; c < 3; ++c) { // RGB only, leave alpha alone
      if (!std::isfinite(p[c]) || p[c] > 65504.0f)
        p[c] = 65504.0f;
      else if (p[c] < 0.0f)
        p[c] = 0.0f; // BC6H_UFLOAT can't represent negatives
    }
  }

  struct StbiGuard {
    float *ptr;
    explicit StbiGuard(float *p) : ptr(p) {}
    ~StbiGuard() { stbi_image_free(ptr); }
    StbiGuard(const StbiGuard &) = delete;
    StbiGuard &operator=(const StbiGuard &) = delete;
  } stbi_guard{data};

  const u32 base_w = static_cast<u32>(width);
  const u32 base_h = static_cast<u32>(height);
  const u32 num_levels =
      static_cast<u32>(std::bit_width(std::max(base_w, base_h)));

  // BasisU HDR encoding requires VK_FORMAT_UNDEFINED — the internal
  // format is managed by the BasisU encoder, not the vkFormat field.
  const ktxTextureCreateInfo create_info{
      .vkFormat = VK_FORMAT_R16G16B16A16_SFLOAT,
      .baseWidth = base_w,
      .baseHeight = base_h,
      .baseDepth = 1,
      .numDimensions = 2,
      .numLevels = num_levels,
      .numLayers = 1,
      .numFaces = 1,
      .isArray = KTX_FALSE,
      .generateMipmaps = KTX_FALSE,
  };

  ktxTexture2 *ktx{};
  if (const auto err = ktxTexture2_Create(
          &create_info, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &ktx);
      err != KTX_SUCCESS) {
    return std::unexpected(
        std::format("ktxTexture2_Create failed: {}", ktx_error_string(err)));
  }
  KtxTexture2Guard guard{ktx};

  u32 src_w = base_w;
  u32 src_h = base_h;
  std::vector<float> prev(data, data + src_w * src_h * 4);

  for (u32 level = 0; level < num_levels; ++level) {
    const usize n = static_cast<usize>(src_w) * src_h * 4;
    std::vector<u16> f16(n);
    for (usize i = 0; i < n; ++i) {
      f16[i] = glm::packHalf1x16(prev[i]);
    }

    if (const auto err = ktxTexture_SetImageFromMemory(
            ktxTexture(ktx), level, 0, 0,
            reinterpret_cast<const ktx_uint8_t *>(f16.data()),
            f16.size() * sizeof(u16));
        err != KTX_SUCCESS) {
      return std::unexpected(
          std::format("SetImageFromMemory failed at level {}: {}", level,
                      ktx_error_string(err)));
    }

    if (level + 1 < num_levels) {
      const u32 dst_w = std::max(1u, src_w / 2);
      const u32 dst_h = std::max(1u, src_h / 2);
      std::vector<float> next(dst_w * dst_h * 4);
      for (u32 y = 0; y < dst_h; ++y) {
        for (u32 x = 0; x < dst_w; ++x) {
          const u32 sx = x * 2, sy = y * 2;
          const u32 sx1 = std::min(sx + 1, src_w - 1);
          const u32 sy1 = std::min(sy + 1, src_h - 1);
          for (u32 c = 0; c < 4; ++c) {
            next[(y * dst_w + x) * 4 + c] =
                (prev[(sy * src_w + sx) * 4 + c] +
                 prev[(sy * src_w + sx1) * 4 + c] +
                 prev[(sy1 * src_w + sx) * 4 + c] +
                 prev[(sy1 * src_w + sx1) * 4 + c]) *
                0.25f;
          }
        }
      }
      prev = std::move(next);
      src_w = dst_w;
      src_h = dst_h;
    }
  }

  // v5 API: `uastc` bool replaced by `codec` enum; `compressionLevel`
  // renamed to `etc1sCompressionLevel`.
  ktxBasisParams bp{};
  bp.structSize = sizeof(bp);
  bp.codec = KTX_BASIS_CODEC_UASTC_HDR_4x4;
  bp.uastcFlags = KTX_PACK_UASTC_LEVEL_DEFAULT;
  bp.etc1sCompressionLevel = KTX_ETC1S_DEFAULT_COMPRESSION_LEVEL;
  bp.threadCount = 8;

  if (const auto err = ktxTexture2_CompressBasisEx(ktx, &bp);
      err != KTX_SUCCESS) {
    return std::unexpected(std::format("ktxTexture2_CompressBasisEx failed: {}",
                                       ktx_error_string(err)));
  }

  // Zstd supercompression on top of the UASTC HDR payload.
  if (const auto err = ktxTexture2_DeflateZstd(ktx, 6); err != KTX_SUCCESS) {
    return std::unexpected(std::format("ktxTexture2_DeflateZstd failed: {}",
                                       ktx_error_string(err)));
  }

  const auto writer = std::format("Dockyard - 1.0.0");
  ktxHashList_AddKVPair(&ktx->kvDataHead, KTX_WRITER_KEY,
                        static_cast<ktx_uint32_t>(writer.size() + 1),
                        writer.data());

  const auto pixel_count = static_cast<usize>(width) * height;
  const auto *pixel_data = reinterpret_cast<const FloatPixel *>(data);
  std::span<const FloatPixel> source_pixels(pixel_data, pixel_count);
  auto [scale_factor, shader_multiplier] = calculate_hdr_scale(source_pixels);
  // Format your calculated scale factor as a string
  const auto hdr_scale_str = std::format(
      "{:.6f}", scale_factor); // Replace 1.0f with your actual calculated scale

  // Inject into the KTX2 hash list
  ktxHashList_AddKVPair(&ktx->kvDataHead, "HDRScale",
                        static_cast<ktx_uint32_t>(hdr_scale_str.size() + 1),
                        hdr_scale_str.data());

  if (const auto err =
          ktxTexture_WriteToNamedFile(ktxTexture(ktx), output.string().c_str());
      err != KTX_SUCCESS) {
    return std::unexpected(
        std::format("WriteToNamedFile failed: {}", ktx_error_string(err)));
  }

  return {};
}

} // namespace

auto convert_hdr_to_ktx2(const VFSPath &input, const VFSPath &output,
                         bool force) -> std::expected<void, std::string> {
  const auto resolved_output = VFS::get().resolve(output);
  if (!force && std::filesystem::exists(resolved_output)) {
    info("Already converted.");
    return {};
  }
  return convert_hdr_to_ktx2(VFS::get().resolve(input), resolved_output);
}

} // namespace dy

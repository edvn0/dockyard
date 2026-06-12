#include <dockyard/texture.hpp>

#include <bit>
#include <cassert>
#include <cstring>
#include <string>

#include <dockyard/app.hpp>
#include <dockyard/context.hpp>
#include <dockyard/vfs.hpp>
#include <dockyard/texture_upload_pool.hpp>

#include <ktx.h>
#include <ktxvulkan.h>

#include <stb_image.h>
#include <stb_image_write.h>
#include <glm/gtc/packing.hpp>
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

  explicit KtxTexture2Guard(ktxTexture2* tex) : texture(tex) {}
  KtxTexture2Guard() = default;

  KtxTexture2Guard(KtxTexture2Guard &&other) noexcept : texture(other.release()) {}

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

  [[nodiscard]] ktxTexture2* release() noexcept {
    ktxTexture2* result = texture;
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
  return (props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
}

[[nodiscard]] auto transcode_ktx2_hdr_if_needed(const VulkanContext &ctx,
                                                ktxTexture2 *ktx)
    -> std::expected<VkFormat, std::string> {
  if (ktxTexture2_NeedsTranscoding(ktx)) {
    return std::unexpected(
        "HDR KTX2 textures must be pre-encoded as BC6H_UFLOAT_BLOCK; "
        "BasisU/UASTC HDR transcoding is not supported"
    );
  }

  const auto format = static_cast<VkFormat>(ktx->vkFormat);

  if (format == VK_FORMAT_UNDEFINED) {
    return std::unexpected(
        "KTX2 has VK_FORMAT_UNDEFINED and does not require transcoding"
    );
  }

  if (!is_sampled_format_supported(ctx, format)) {
    return std::unexpected(std::format(
        "KTX2 VkFormat {} is not supported as sampled optimal-tiled image",
        std::to_underlying(format)
    ));
  }

  return format;
}

[[nodiscard]] auto decode_ktx2_hdr_texture(const VulkanContext &ctx,
                                           const std::filesystem::path &path)
    -> std::expected<KtxHdrDecodeResult, std::string> {
  ktxTexture2 *raw_ktx{};

  const auto create_error = ktxTexture2_CreateFromNamedFile(
      path.string().c_str(),
      KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,
      &raw_ktx
  );

  if (create_error != KTX_SUCCESS) {
    return std::unexpected(std::format(
        "ktxTexture2_CreateFromNamedFile failed: {}",
        ktx_error_string(create_error)
    ));
  }

  KtxTexture2Guard guard{raw_ktx};
  auto *ktx = guard.texture;

  if (ktx->numDimensions != 2) {
    return std::unexpected(std::format(
        "Expected 2D KTX2 HDR equirect texture, got {} dimensions",
        ktx->numDimensions
    ));
  }

  if (ktx->numFaces != 1) {
    return std::unexpected(std::format(
        "Expected equirect KTX2 texture with 1 face, got {} faces",
        ktx->numFaces
    ));
  }

  if (ktx->numLayers > 1) {
    return std::unexpected(std::format(
        "Expected non-array KTX2 texture, got {} layers",
        ktx->numLayers
    ));
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
      return std::unexpected(std::format(
          "ktxTexture_GetImageOffset failed for mip {}: {}",
          level,
          ktx_error_string(offset_error)
      ));
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

[[nodiscard]] auto lower_extension(std::filesystem::path path) -> std::string {
  auto ext = path.extension().string();
  std::ranges::transform(ext, ext.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return ext;
}

} // namespace

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
  case VK_FORMAT_BC7_SRGB_BLOCK:
  case VK_FORMAT_BC7_UNORM_BLOCK:
  case VK_FORMAT_BC5_UNORM_BLOCK:
  case VK_FORMAT_BC6H_UFLOAT_BLOCK:
  case VK_FORMAT_BC6H_SFLOAT_BLOCK:
    // 16 bytes per 4x4 block; sizing via this value is only valid when
    // the caller supplies pre-sized byte spans (e.g. from libktx).
    return 16;
  default:
    info("Warning: bytes_per_texel: unknown format {}, defaulting to 4",
         std::to_underlying(fmt));
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
                                     const VFSPath &path) -> Texture {
  const auto real_path = VFS::get().resolve(path);

  auto result = decode_ktx2_hdr_texture(ctx, real_path);
  if (!result) {
    error("Failed to decode KTX2 HDR texture '{}': {}",
          real_path.string(), result.error());
    std::abort();
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
        .width  = decoded.mips[i].width,
        .height = decoded.mips[i].height,
    });
  }

  return Texture::from_bytes(ctx, real_path.filename().string(), {
      .bytes         = base_bytes,
      .mips          = extra_mips,
      .width         = decoded.width,
      .height        = decoded.height,
      .format        = decoded.vk_format,
      .generate_mips = false,
      .storage_view  = false,
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

  const bool transient = samples > VK_SAMPLE_COUNT_1_BIT && ctx.caps.transient_attachments;
  if (transient) {
    image_info.usage |= VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
  }

  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  VmaAllocationCreateInfo vma_info{};
  vma_info.usage = transient ? VMA_MEMORY_USAGE_GPU_LAZILY_ALLOCATED
                             : VMA_MEMORY_USAGE_AUTO;

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

  ctx.transition_to_general(rt.image, aspect, mips, 1);

  return rt;
}

auto Texture::from_bytes(const VulkanContext &ctx, std::string_view name,
                         const CreateInfo &ci) -> Texture {
  assert((!ci.bytes.empty() || !ci.mips.empty()) && ci.width > 0 &&
         ci.height > 0);

  const bool has_custom_mips = !ci.mips.empty();

  u32 mips = 1u;
  VkDeviceSize total_byte_size = 0;
  const u32 bytes_per_pixel = bytes_per_texel(ci.format);

  struct MipCopyRegion {
    VkDeviceSize buffer_offset;
    u32 width;
    u32 height;
  };
  std::vector<MipCopyRegion> copy_regions;

  if (has_custom_mips) {
    mips = static_cast<u32>(ci.mips.size() + 1);
    copy_regions.reserve(mips);

    total_byte_size =
        static_cast<VkDeviceSize>(ci.bytes.size_bytes());
    copy_regions.push_back({0, ci.width, ci.height});

    VkDeviceSize current_offset = total_byte_size;
    for (const auto &mip : ci.mips) {
      copy_regions.push_back({current_offset, mip.width, mip.height});
      current_offset += static_cast<VkDeviceSize>(mip.pixels.size());
    }
    total_byte_size = current_offset;
  } else {
    mips = ci.generate_mips ? mip_count(ci.width, ci.height) : 1u;
    total_byte_size =
        static_cast<VkDeviceSize>(ci.width) * ci.height * bytes_per_pixel;
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
  {auto staging_name = std::format("{}_staging_buffer", name);
  VkDebugUtilsObjectNameInfoEXT name_info{
      .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
      .objectType = VK_OBJECT_TYPE_BUFFER,
      .objectHandle = std::bit_cast<u64>(staging_buf),
      .pObjectName = staging_name.c_str(),
  };
  vkSetDebugUtilsObjectNameEXT(ctx.device, &name_info);}

  auto *dst_ptr = static_cast<u8 *>(staging_info.pMappedData);
  if (has_custom_mips) {
    std::memcpy(dst_ptr + copy_regions[0].buffer_offset,
                ci.bytes.data(), ci.bytes.size_bytes());
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
      .flags = ci.dedicated_memory ? VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT : 0u,
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

{  const VkDebugUtilsObjectNameInfoEXT name_info{
      .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
      .objectType = VK_OBJECT_TYPE_IMAGE,
      .objectHandle = std::bit_cast<u64>(tex.image),
      .pObjectName = tex.name.c_str(),
  };
  vkSetDebugUtilsObjectNameEXT(ctx.device, &name_info);}

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
      .flags = ci.dedicated_memory ? VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT : 0u,
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
auto convert_hdr_to_ktx2(const std::filesystem::path &input,
                         const std::filesystem::path &output)
    -> std::expected<void, std::string> {
  int width{};
  int height{};
  int channels{};
  float *data = stbi_loadf(input.string().c_str(), &width, &height, &channels, 4);
  if (data == nullptr) {
    return std::unexpected(
        std::format("stbi_loadf failed: {}", stbi_failure_reason()));
  }

  struct StbiGuard {
    float *ptr;
    explicit StbiGuard(float *p) : ptr(p) {}
    ~StbiGuard() { stbi_image_free(ptr); }
    StbiGuard(const StbiGuard &) = delete;
    StbiGuard &operator=(const StbiGuard &) = delete;
    StbiGuard(StbiGuard &&other) noexcept : ptr(std::exchange(other.ptr, nullptr)) {}
    StbiGuard &operator=(StbiGuard &&other) noexcept {
      if (this != &other) {
        stbi_image_free(ptr);
        ptr = std::exchange(other.ptr, nullptr);
      }
      return *this;
    }
  } stbi_guard(data);

  const u32 num_levels = static_cast<u32>(
      std::bit_width(static_cast<u32>(std::max(width, height))));

  ktxTextureCreateInfo create_info{
      .vkFormat = VK_FORMAT_R16G16B16A16_SFLOAT,
      .baseWidth = static_cast<u32>(width),
      .baseHeight = static_cast<u32>(height),
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
        std::format("ktxTexture2_Create failed: {}", std::to_underlying(err)));
  }
  KtxTexture2Guard guard{ktx};

  // Write level 0 directly into libktx's allocated slab.
  {
    const auto pixel_count = static_cast<usize>(width) * height;
    std::vector<u16> f16(pixel_count * 4);
    for (usize i = 0; i < f16.size(); ++i) {
      f16[i] = glm::packHalf1x16(data[i]);
    }

    ktx_size_t offset{};
    ktxTexture_GetImageOffset(ktxTexture(ktx), 0, 0, 0, &offset);
    std::memcpy(ktxTexture_GetData(ktxTexture(ktx)) + offset, f16.data(),
                f16.size() * sizeof(u16));
  }

  // Build + write mip chain, keeping f32 intermediates to avoid error
  // accumulation.
  {
    u32 src_w = static_cast<u32>(width);
    u32 src_h = static_cast<u32>(height);
    std::vector<float> prev_f32(data, data + src_w * src_h * 4);

    for (u32 level = 1; level < num_levels; ++level) {
      const u32 dst_w = std::max(1u, src_w / 2);
      const u32 dst_h = std::max(1u, src_h / 2);

      std::vector<float> dst_f32(dst_w * dst_h * 4);

      for (u32 y = 0; y < dst_h; ++y) {
        for (u32 x = 0; x < dst_w; ++x) {
          const u32 sx = x * 2;
          const u32 sy = y * 2;
          const u32 sx1 = std::min(sx + 1, src_w - 1);
          const u32 sy1 = std::min(sy + 1, src_h - 1);

          for (u32 c = 0; c < 4; ++c) {
            dst_f32[(y * dst_w + x) * 4 + c] =
                (prev_f32[(sy  * src_w + sx ) * 4 + c] +
                 prev_f32[(sy  * src_w + sx1) * 4 + c] +
                 prev_f32[(sy1 * src_w + sx ) * 4 + c] +
                 prev_f32[(sy1 * src_w + sx1) * 4 + c]) * 0.25f;
          }
        }
      }

      std::vector<u16> f16_mip(dst_w * dst_h * 4);
      for (usize i = 0; i < f16_mip.size(); ++i) {
        f16_mip[i] = glm::packHalf1x16(dst_f32[i]);
      }

      ktx_size_t offset{};
      ktxTexture_GetImageOffset(ktxTexture(ktx), level, 0, 0, &offset);
      std::memcpy(ktxTexture_GetData(ktxTexture(ktx)) + offset, f16_mip.data(),
                  f16_mip.size() * sizeof(u16));

      prev_f32 = std::move(dst_f32);
      src_w = dst_w;
      src_h = dst_h;
    }
  }

  const auto name = output.string();
  const auto name_cstr = name.c_str();
  char writer[100];
  snprintf(writer, sizeof(writer), "%s version %s", "Dockyard", "1.0.0");
  ktxHashList_AddKVPair(&ktx->kvDataHead, KTX_WRITER_KEY,
                        static_cast<ktx_uint32_t>(strlen(writer)) + 1, writer);
  if (const auto err = ktxTexture_WriteToNamedFile(ktxTexture(ktx), name_cstr);
      err != KTX_SUCCESS) {
    return std::unexpected(std::format("ktxTexture_WriteToNamedFile failed: {}",
                                       std::to_underlying(err)));
  }

  return {};
}
} // namespace

auto convert_hdr_to_ktx2(const VFSPath &input, const VFSPath &output)
    -> std::expected<void, std::string> {
  return convert_hdr_to_ktx2(VFS::get().resolve(input),
                             VFS::get().resolve(output));
}

} // namespace dy

#include "dockyard/bindless_handle.hpp"
#include <dockyard/context.hpp>

#include <volk.h>

#include <dockyard/app.hpp>
#include <dockyard/buffer.hpp>
#include <dockyard/events.hpp>
#include <dockyard/log.hpp>
#include <dockyard/pipeline_builder.hpp>
#include <dockyard/scene_renderer.hpp>
#include <dockyard/vfs.hpp>
#include <dockyard/vk_check.hpp>

#include <array>
#include <optional>

namespace dy {

namespace {
constexpr u32 brdf_lut_size = 512u;
constexpr u32 brdf_lut_bytes_per_pixel = 4u; // VK_FORMAT_R16G16_SFLOAT
constexpr u32 brdf_lut_cache_magic = 0x55524C42u; // 'BLRU'
constexpr u32 brdf_lut_cache_version = 1u;

// Sources that affect the generated LUT contents — cache is invalidated
// whenever any of these change, so shader edits never serve stale data.
constexpr std::array brdf_lut_source_paths = {
    "shaders://ibl/brdf_lut_gen.slang",
    "shaders://include/ibl.slang",
    "shaders://include/pbr.slang",
    "shaders://include/constants.slang",
};

struct BrdfLutCacheHeader {
  u32 magic;
  u32 version;
  u32 width;
  u32 height;
  u64 source_hash;
};

[[nodiscard]] auto hash_brdf_lut_sources() -> u64 {
  u64 h = 14695981039346656037ULL;
  for (const char *path : brdf_lut_source_paths) {
    auto bytes = VFS::get().read_bytes(VFSPath::create(path));
    if (!bytes)
      continue;
    h ^= hash_bytes(bytes->data(), bytes->size());
    h *= 1099511628211ULL;
  }
  return h;
}

[[nodiscard]] auto brdf_lut_cache_path() -> VFSPath {
  return VFSPath::create("binary://brdf_lut_{}.cache", brdf_lut_size);
}

[[nodiscard]] auto load_cached_brdf_lut(u64 expected_hash)
    -> std::optional<std::vector<std::byte>> {
  auto stream = VFS::get().resolve_to_input_stream(brdf_lut_cache_path());
  if (!stream)
    return std::nullopt;

  BrdfLutCacheHeader header{};
  stream->read(reinterpret_cast<char *>(&header), sizeof(header));
  if (!*stream || header.magic != brdf_lut_cache_magic ||
      header.version != brdf_lut_cache_version ||
      header.width != brdf_lut_size || header.height != brdf_lut_size ||
      header.source_hash != expected_hash)
    return std::nullopt;

  const usize pixel_bytes =
      usize{header.width} * header.height * brdf_lut_bytes_per_pixel;
  std::vector<std::byte> pixels(pixel_bytes);
  stream->read(reinterpret_cast<char *>(pixels.data()),
              static_cast<std::streamsize>(pixel_bytes));
  if (!*stream)
    return std::nullopt;
  return pixels;
}

auto save_cached_brdf_lut(u64 source_hash, std::span<const std::byte> pixels)
    -> void {
  auto stream = VFS::get().resolve_to_output_stream(brdf_lut_cache_path());
  if (!stream) {
    warn("Failed to write BRDF LUT cache: {}", stream.error());
    return;
  }
  const BrdfLutCacheHeader header{
      .magic = brdf_lut_cache_magic,
      .version = brdf_lut_cache_version,
      .width = brdf_lut_size,
      .height = brdf_lut_size,
      .source_hash = source_hash,
  };
  stream->write(reinterpret_cast<const char *>(&header), sizeof(header));
  stream->write(reinterpret_cast<const char *>(pixels.data()),
               static_cast<std::streamsize>(pixels.size()));
}
} // namespace

auto RendererListener::on_swapchain_invalidated(
    const events::SwapchainInvalidated &e) const -> void {

  vkDeviceWaitIdle(ctx.device);

  if (e.width == 0 || e.height == 0) {
    minimized = true;
    return;
  }
  minimized = false;
  sc.rebuild(ctx, surface, e.width, e.height);

  frame_index = 0;
  frame_ids.assign(sc.swapchain.image_count, 0);
  for (auto &fs : frames.frame_sync) {
    fs.last_value = 0;
    fs.destroy(ctx);
    fs = FrameSync::create(ctx);
  }

  dispatch.trigger<events::SwapchainResized>(events::SwapchainResized{
      .width = e.width,
      .height = e.height,
  });
}

auto RendererListener::on_window_minimized(const events::WindowMinimized &e)
    -> void {
  minimized = e.minimized;
}

auto FrameResources::destroy(const VulkanContext &ctx) -> void {
  for (auto &cb : command_buffers)
    cb.destroy(ctx);
  for (auto &fs : frame_sync)
    fs.destroy(ctx);
}
auto FrameResources::create(const VulkanContext &ctx) -> FrameResources {
  FrameResources r{};
  r.command_buffers.resize(frames_in_flight);
  r.frame_sync.resize(frames_in_flight);
  for (auto &cb : r.command_buffers)
    cb = CommandBuffer::create(ctx);
  for (auto &fs : r.frame_sync)
    fs = FrameSync::create(ctx);
  return r;
}

auto SwapchainResources::rebuild(const VulkanContext &ctx, VkSurfaceKHR surface,
                                 u32 width, u32 height) -> void {
  for (auto &view : image_views)
    vkDestroyImageView(ctx.device, view, nullptr);
  image_views.clear();

  for (auto &is : image_sync)
    is.destroy(ctx);
  image_sync.clear();

  auto maybe_new_swapchain =
      vkb::SwapchainBuilder{ctx.physical_device, ctx.device, surface,
                            ctx.graphics_queue_index, ctx.present_queue_index}
          .set_desired_extent(width, height)
          .set_old_swapchain(swapchain)
          .use_default_format_selection()
          .set_desired_present_mode(VK_PRESENT_MODE_MAILBOX_KHR)
          .add_fallback_present_mode(VK_PRESENT_MODE_FIFO_KHR)
          .set_desired_min_image_count(frames_in_flight + 5) // 3
          .set_image_usage_flags(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
          .build();

  if (!maybe_new_swapchain) {
    info("Swapchain creation failed: {}, {}",
         maybe_new_swapchain.error().message(),
         maybe_new_swapchain.detailed_failure_reasons());
    std::abort();
  }
  auto new_swapchain = std::move(maybe_new_swapchain.value());

  vkb::destroy_swapchain(swapchain);
  swapchain = std::move(new_swapchain);
  images = swapchain.get_images().value();
  image_views = swapchain.get_image_views().value();

  image_sync.resize(swapchain.image_count);
  for (auto &is : image_sync)
    is = ImageSync::create(ctx);

  trace("Swapchain rebuilt: {}x{}, image count: {}", width, height,
        swapchain.image_count);
}

auto SwapchainResources::destroy(const VulkanContext &ctx) -> void {
  for (auto &view : image_views)
    vkDestroyImageView(ctx.device, view, nullptr);
  for (auto &is : image_sync)
    is.destroy(ctx);
  vkb::destroy_swapchain(swapchain);
}
auto SwapchainResources::create(const VulkanContext &ctx, VkSurfaceKHR surface,
                                u32 width, u32 height) -> SwapchainResources {
  SwapchainResources r{};
  r.rebuild(ctx, surface, width, height);
  return r;
}

auto ViewportResources::resize(const VulkanContext &ctx,
                               SceneRenderer &renderer, u32 w, u32 h) -> void {
  depth_msaa.destroy(ctx);
  depth_msaa = Texture::create(ctx, "depth_msaa", w, h, VK_FORMAT_D32_SFLOAT,
                               VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                               VK_IMAGE_ASPECT_DEPTH_BIT, VK_SAMPLE_COUNT_4_BIT,
                               1U, true);

  forward_target_msaa.destroy(ctx);
  forward_target_msaa = Texture::create(
      ctx, "forward_msaa", w, h, VK_FORMAT_R16G16B16A16_SFLOAT,
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
      VK_SAMPLE_COUNT_4_BIT, 1U, true);

  auto resize_or_create = [&](auto &handle, auto make_tex) {
    if (handle.valid()) {
      auto *entry = renderer.textures.get(handle);
      entry->texture.destroy(ctx, &renderer.textures);
      entry->texture = make_tex();
      renderer.bindless.mark_dirty();
    } else {
      handle = renderer.textures.create(TextureEntry{
          .texture = make_tex(),
          .sampled_view_type = VK_IMAGE_VIEW_TYPE_2D,
      });
    }
  };

  for (u32 i = 0; i < frames_in_flight; ++i) {
    resize_or_create(depth_resolved_target[i], [&] {
      return Texture::create(
          ctx, "depth_resolved_target", w, h, VK_FORMAT_D32_SFLOAT,
          VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
              VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
          VK_IMAGE_ASPECT_DEPTH_BIT, VK_SAMPLE_COUNT_1_BIT, 1U, true);
    });

    resize_or_create(depth_pre_hiz[i], [&] {
      return Texture::create(
          ctx, "depth_pre_hiz", w, h, VK_FORMAT_R32_SFLOAT,
          VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
              VK_IMAGE_USAGE_STORAGE_BIT,
          VK_IMAGE_ASPECT_COLOR_BIT, VK_SAMPLE_COUNT_1_BIT, 1U, true);
    });
  }

  u32 hiz_w = std::max(1U, w / 2);
  u32 hiz_h = std::max(1U, h / 2);
  u32 hiz_mips = static_cast<u32>(std::bit_width(std::max(hiz_w, hiz_h)));

  for (u32 i = 0; i < frames_in_flight; ++i) {
    auto &pyramid_handle = hierarchical_depth_pyramid_target[i];
    if (pyramid_handle.valid()) {
      renderer.textures.get(pyramid_handle)->texture.destroy(
          ctx, &renderer.textures);

      // Build the new pyramid outside the pool: register_sub_views calls
      // texture_pool.create() which can reallocate pool_slots, invalidating
      // any pointer into the pool obtained before the call.
      auto new_pyramid = Texture::create(
          ctx, "hiz_pyramid_target", hiz_w, hiz_h, VK_FORMAT_R32_SFLOAT,
          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
          VK_IMAGE_ASPECT_COLOR_BIT, VK_SAMPLE_COUNT_1_BIT, hiz_mips, true);
      new_pyramid.register_sub_views(ctx, renderer.textures, renderer.bindless);

      // Re-fetch: pool_slots may have been reallocated during register_sub_views.
      renderer.textures.get(pyramid_handle)->texture = std::move(new_pyramid);
    } else {
      auto tex = Texture::create(
          ctx, "hiz_pyramid_target", hiz_w, hiz_h, VK_FORMAT_R32_SFLOAT,
          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
          VK_IMAGE_ASPECT_COLOR_BIT, VK_SAMPLE_COUNT_1_BIT, hiz_mips, true);
      tex.register_sub_views(ctx, renderer.textures, renderer.bindless);
      pyramid_handle = renderer.textures.create(TextureEntry{
          .texture = std::move(tex),
          .sampled_view_type = VK_IMAGE_VIEW_TYPE_2D,
      });
    }
  }

  resize_or_create(forward_target, [&] {
    return Texture::create(
        ctx, "forward_target", w, h, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_STORAGE_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT, VK_SAMPLE_COUNT_1_BIT, 1U, true);
  });

  {
    const u32 bloom_w = std::max(1U, w / 2);
    const u32 bloom_h = std::max(1U, h / 2);
    if (bloom_chain.valid()) {
      renderer.textures.get(bloom_chain)->texture.destroy(ctx, &renderer.textures);
      auto new_bloom = Texture::create(
          ctx, "bloom_chain", bloom_w, bloom_h, VK_FORMAT_R16G16B16A16_SFLOAT,
          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
          VK_IMAGE_ASPECT_COLOR_BIT, VK_SAMPLE_COUNT_1_BIT,
          SceneRenderer::bloom_mip_count, true);
      new_bloom.register_sub_views(ctx, renderer.textures, renderer.bindless);
      renderer.textures.get(bloom_chain)->texture = std::move(new_bloom);
    } else {
      auto tex = Texture::create(
          ctx, "bloom_chain", bloom_w, bloom_h, VK_FORMAT_R16G16B16A16_SFLOAT,
          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
          VK_IMAGE_ASPECT_COLOR_BIT, VK_SAMPLE_COUNT_1_BIT,
          SceneRenderer::bloom_mip_count, true);
      tex.register_sub_views(ctx, renderer.textures, renderer.bindless);
      bloom_chain = renderer.textures.create(TextureEntry{
          .texture = std::move(tex),
          .sampled_view_type = VK_IMAGE_VIEW_TYPE_2D,
      });
    }

    if (bloom_scratch.valid()) {
      renderer.textures.get(bloom_scratch)->texture.destroy(ctx, &renderer.textures);
      auto new_scratch = Texture::create(
          ctx, "bloom_scratch", bloom_w, bloom_h, VK_FORMAT_R16G16B16A16_SFLOAT,
          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
          VK_IMAGE_ASPECT_COLOR_BIT, VK_SAMPLE_COUNT_1_BIT,
          SceneRenderer::bloom_mip_count, true);
      new_scratch.register_sub_views(ctx, renderer.textures, renderer.bindless);
      renderer.textures.get(bloom_scratch)->texture = std::move(new_scratch);
    } else {
      auto tex = Texture::create(
          ctx, "bloom_scratch", bloom_w, bloom_h, VK_FORMAT_R16G16B16A16_SFLOAT,
          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
          VK_IMAGE_ASPECT_COLOR_BIT, VK_SAMPLE_COUNT_1_BIT,
          SceneRenderer::bloom_mip_count, true);
      tex.register_sub_views(ctx, renderer.textures, renderer.bindless);
      bloom_scratch = renderer.textures.create(TextureEntry{
          .texture = std::move(tex),
          .sampled_view_type = VK_IMAGE_VIEW_TYPE_2D,
      });
    }
  }

  resize_or_create(display_target, [&] {
    return Texture::create(
        ctx, "display_target", w, h, VK_FORMAT_R8G8B8A8_SRGB,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT, VK_SAMPLE_COUNT_1_BIT, 1U, true);
  });
}

auto ViewportResources::destroy(const VulkanContext &ctx) -> void {
  depth_msaa.destroy(ctx);
  forward_target_msaa.destroy(ctx);
}
auto ViewportResources::create(const VulkanContext &ctx,
                               SceneRenderer &renderer, u32 w, u32 h)
    -> ViewportResources {
  ViewportResources r{};
  r.resize(ctx, renderer, w, h);
  return r;
}

auto VulkanContext::graphics_queue() const -> VkQueue {
  return device.get_queue(vkb::QueueType::graphics).value();
}

auto VulkanContext::present_queue() const -> VkQueue {
  return device.get_queue(vkb::QueueType::present).value();
}

auto VulkanContext::destroy() -> void {
  vmaDestroyAllocator(allocator);
  if (surface != VK_NULL_HANDLE) {
    vkDestroySurfaceKHR(instance.instance, surface, nullptr);
  }
  vkb::destroy_device(device);
  vkb::destroy_instance(instance);
}

auto VulkanContext::create(vkb::Instance &&inst, VkSurfaceKHR &&s)
    -> VulkanContext {
  VulkanContext ctx{};
  ctx.surface = s;
  ctx.instance = inst;
  volkLoadInstance(ctx.instance.instance);

  auto bare_ret = vkb::PhysicalDeviceSelector{ctx.instance}
                      .set_surface(ctx.surface)
                      .set_minimum_version(1, 4)
                      .select();
  if (!bare_ret) {
    error("Failed to find a Vulkan 1.4 GPU: {}", bare_ret.error().message());
    std::abort();
  }
  VkPhysicalDevice phys_handle = bare_ret.value().physical_device;

  VkPhysicalDeviceVulkan14Features avail_14{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES,
  };
  VkPhysicalDevicePresentWaitFeaturesKHR avail_present_wait{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR,
      .pNext = &avail_14,
  };
  VkPhysicalDevicePresentIdFeaturesKHR avail_present_id{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR,
      .pNext = &avail_present_wait,
  };
  VkPhysicalDeviceUnifiedImageLayoutsFeaturesKHR avail_unified{
      .sType =
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFIED_IMAGE_LAYOUTS_FEATURES_KHR,
      .pNext = &avail_present_id,
  };
  VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR avail_pipeline_exec{
      .sType =
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR,
      .pNext = &avail_unified,
  };
  VkPhysicalDeviceFaultFeaturesEXT avail_fault{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT,
      .pNext = &avail_pipeline_exec,
  };
  VkPhysicalDeviceFeatures2 avail2{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
      .pNext = &avail_fault,
  };
  vkGetPhysicalDeviceFeatures2(phys_handle, &avail2);

  ctx.caps.maintenance5 = avail_14.maintenance5 == VK_TRUE;
  ctx.caps.maintenance6 = avail_14.maintenance6 == VK_TRUE;
  ctx.caps.smooth_lines = avail_14.smoothLines == VK_TRUE;
  ctx.caps.stippled_smooth_lines = avail_14.stippledSmoothLines == VK_TRUE;
  ctx.caps.push_descriptor = avail_14.pushDescriptor == VK_TRUE;
  ctx.caps.present_wait = avail_present_wait.presentWait == VK_TRUE;
  ctx.caps.unified_image_layouts = avail_unified.unifiedImageLayouts == VK_TRUE;
  ctx.caps.executable_properties =
      avail_pipeline_exec.pipelineExecutableInfo == VK_TRUE;
  // ctx.caps.device_fault = avail_fault.deviceFault == VK_TRUE;

  {
    VkPhysicalDeviceMemoryProperties mem_props{};
    vkGetPhysicalDeviceMemoryProperties(phys_handle, &mem_props);
    for (u32 i = 0; i < mem_props.memoryTypeCount; ++i) {
      if (mem_props.memoryTypes[i].propertyFlags &
          VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) {
        ctx.caps.transient_attachments = true;
        break;
      }
    }
  }

  info("Caps: maintenance5={} maintenance6={} smooth_lines={} "
       "stippled_smooth_lines={} push_descriptor={} present_wait={} "
       "unified_image_layouts={} transient_attachments={} device_fault={}",
       ctx.caps.maintenance5, ctx.caps.maintenance6, ctx.caps.smooth_lines,
       ctx.caps.stippled_smooth_lines, ctx.caps.push_descriptor,
       ctx.caps.present_wait, ctx.caps.unified_image_layouts,
       ctx.caps.transient_attachments, ctx.caps.device_fault);

  VkPhysicalDeviceFeatures features{};
  features.multiDrawIndirect = VK_TRUE;
  features.samplerAnisotropy = VK_TRUE;
  features.textureCompressionBC = VK_TRUE;
  features.wideLines = VK_TRUE;
  features.shaderInt64 = VK_TRUE;
  features.drawIndirectFirstInstance = VK_TRUE;

  VkPhysicalDeviceVulkan11Features features_11{};
  features_11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
  features_11.storageBuffer16BitAccess = VK_TRUE;
  features_11.variablePointersStorageBuffer = VK_TRUE;
  features_11.variablePointers = VK_TRUE;
  features_11.shaderDrawParameters = VK_TRUE;
  features_11.multiview = VK_TRUE;

  VkPhysicalDeviceVulkan12Features features_12{};
  features_12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
  features_12.drawIndirectCount = VK_TRUE;
  features_12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
  features_12.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
  features_12.shaderStorageImageArrayNonUniformIndexing = VK_TRUE;
  features_12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
  features_12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
  features_12.descriptorBindingPartiallyBound = VK_TRUE;
  features_12.descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;
  features_12.descriptorBindingVariableDescriptorCount = VK_TRUE;
  features_12.runtimeDescriptorArray = VK_TRUE;
  features_12.scalarBlockLayout = VK_TRUE;
  features_12.timelineSemaphore = VK_TRUE;
  features_12.bufferDeviceAddress = VK_TRUE;
  features_12.samplerFilterMinmax = VK_TRUE;
  features_12.hostQueryReset = VK_TRUE;

  VkPhysicalDeviceVulkan13Features features_13{};
  features_13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
  features_13.shaderDemoteToHelperInvocation = VK_TRUE;
  features_13.synchronization2 = VK_TRUE;
  features_13.dynamicRendering = VK_TRUE;
  features_13.maintenance4 = VK_TRUE;

  VkPhysicalDeviceVulkan14Features features_14{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES,
      .smoothLines = ctx.caps.smooth_lines ? VK_TRUE : VK_FALSE,
      .stippledSmoothLines =
          ctx.caps.stippled_smooth_lines ? VK_TRUE : VK_FALSE,
      .dynamicRenderingLocalRead = VK_TRUE,
      .maintenance5 = ctx.caps.maintenance5 ? VK_TRUE : VK_FALSE,
      .maintenance6 = ctx.caps.maintenance6 ? VK_TRUE : VK_FALSE,
      .pushDescriptor = ctx.caps.push_descriptor ? VK_TRUE : VK_FALSE,
  };

  VkPhysicalDevicePresentIdFeaturesKHR present_id_features{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR,
      .pNext = nullptr,
      .presentId = VK_TRUE,
  };
  VkPhysicalDevicePresentWaitFeaturesKHR present_wait_features{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR,
      .pNext = nullptr,
      .presentWait = VK_TRUE,
  };
  VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR
      pipeline_executable_features{
          .sType =
              VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR,
          .pNext = nullptr,
          .pipelineExecutableInfo = VK_TRUE,
  };

  auto selector = vkb::PhysicalDeviceSelector{ctx.instance}
                      .set_surface(ctx.surface)
                      .set_minimum_version(1, 4)
                      .set_required_features(features)
                      .set_required_features_11(features_11)
                      .set_required_features_12(features_12)
                      .set_required_features_13(features_13)
                      .set_required_features_14(features_14);

  selector = selector.add_required_extensions({
      VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME,
  });

  if (ctx.caps.present_wait) {
    selector = selector.add_required_extension_features(present_id_features)
                   .add_required_extension_features(present_wait_features)
                   .add_required_extensions({
                       VK_KHR_PRESENT_ID_EXTENSION_NAME,
                       VK_KHR_PRESENT_WAIT_EXTENSION_NAME,
                   });
  }
  if (ctx.caps.unified_image_layouts) {
    selector = selector.add_required_extensions({
        VK_KHR_UNIFIED_IMAGE_LAYOUTS_EXTENSION_NAME,
    });
  }
  if (ctx.caps.executable_properties) {
    selector =
        selector.add_required_extension_features(pipeline_executable_features)
            .add_required_extensions({
                VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME,
            });
  }
  VkPhysicalDeviceFaultFeaturesEXT fault_features{
      .sType       = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT,
      .deviceFault = VK_TRUE,
  };
  if (ctx.caps.device_fault) {
    selector = selector.add_required_extension_features(fault_features)
                   .add_required_extensions({
                       VK_EXT_DEVICE_FAULT_EXTENSION_NAME,
                   });
  }

  auto phys_ret = selector.select();
  if (!phys_ret) {
    error("Failed to select physical device: {}", phys_ret.error().message());
    std::abort();
  }
  ctx.physical_device = std::move(phys_ret.value());
  info("Selected GPU: {}", ctx.physical_device.name);

  auto dev_ret = vkb::DeviceBuilder{ctx.physical_device}.build();
  if (!dev_ret) {
    error("Failed to create device: {}", dev_ret.error().message());
    std::abort();
  }
  ctx.device = std::move(dev_ret.value());
  volkLoadDevice(ctx.device.device);

  ctx.graphics_queue_index =
      ctx.device.get_queue_index(vkb::QueueType::graphics).value();
  ctx.present_queue_index =
      ctx.device.get_queue_index(vkb::QueueType::present).value();
  info("Graphics queue: {}, Present queue: {}", ctx.graphics_queue_index,
       ctx.present_queue_index);

  VmaVulkanFunctions vma_fns{};
  VmaAllocatorCreateInfo alloc_info{};
  alloc_info.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
  alloc_info.physicalDevice = ctx.physical_device;
  alloc_info.device = ctx.device;
  alloc_info.pVulkanFunctions = &vma_fns;
  alloc_info.instance = ctx.instance;
  alloc_info.vulkanApiVersion = VK_API_VERSION_1_4;
  vmaImportVulkanFunctionsFromVolk(&alloc_info, &vma_fns);

  if (const auto result = vmaCreateAllocator(&alloc_info, &ctx.allocator);
      result != VK_SUCCESS) {
    error("Failed to create VMA allocator: {}", result);
    std::abort();
  }
  info("Allocator created");

  return ctx;
}

auto FrameSync::create(const VulkanContext &ctx) -> FrameSync {
  FrameSync fs{};

  VkSemaphoreTypeCreateInfo timeline_type_info{};
  timeline_type_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
  timeline_type_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
  timeline_type_info.initialValue = 0;

  VkSemaphoreCreateInfo sem_info{};
  sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  sem_info.pNext = &timeline_type_info;

  vkCreateSemaphore(ctx.device, &sem_info, nullptr, &fs.timeline_semaphore);

  sem_info.pNext = nullptr; // Reset pNext for binary semaphore
  vkCreateSemaphore(ctx.device, &sem_info, nullptr,
                    &fs.image_available_semaphore);

  VkFenceCreateInfo fence_info{};
  fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  vkCreateFence(ctx.device, &fence_info, nullptr, &fs.in_flight_fence);

  fs.last_value = 0;
  return fs;
}

auto FrameSync::destroy(const VulkanContext &ctx) -> void {
  vkDestroySemaphore(ctx.device, timeline_semaphore, nullptr);
  vkDestroySemaphore(ctx.device, image_available_semaphore, nullptr);
  vkDestroyFence(ctx.device, in_flight_fence, nullptr);
}

auto ImageSync::destroy(const VulkanContext &ctx) -> void {
  vkDestroySemaphore(ctx.device, render_finished_semaphore, nullptr);
}
auto ImageSync::create(const VulkanContext &ctx) -> ImageSync {
  ImageSync is{};
  VkSemaphoreCreateInfo sem_info{};
  sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  vkCreateSemaphore(ctx.device, &sem_info, nullptr,
                    &is.render_finished_semaphore);
  return is;
}
auto CommandBuffer::reset(const VulkanContext &ctx) -> void {
  vkResetCommandPool(ctx.device, command_pool, 0);
}
auto CommandBuffer::begin(VkCommandBufferUsageFlags flags) -> bool {
  VkCommandBufferBeginInfo begin_info{};
  begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin_info.flags = flags;
  const auto r = vkBeginCommandBuffer(command_buffer, &begin_info);
  if (r != VK_SUCCESS) {
    error("Failed to begin command buffer: {}", r);
    return false;
  }
  return true;
}
auto CommandBuffer::end() -> bool {
  const auto r = vkEndCommandBuffer(command_buffer);
  if (r != VK_SUCCESS) {
    error("Failed to end command buffer: {}", r);
    return false;
  }
  return true;
}
auto CommandBuffer::destroy(const VulkanContext &ctx) -> void {
  vkFreeCommandBuffers(ctx.device, command_pool, 1, &command_buffer);
  vkDestroyCommandPool(ctx.device, command_pool, nullptr);
}
auto CommandBuffer::create(const VulkanContext &ctx) -> CommandBuffer {
  CommandBuffer cb{};
  VkCommandPoolCreateInfo pool_info{};
  pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pool_info.queueFamilyIndex = ctx.graphics_queue_index;
  pool_info.flags = 0;
  if (const auto result = vkCreateCommandPool(ctx.device, &pool_info, nullptr,
                                              &cb.command_pool);
      result != VK_SUCCESS) {
    error("Failed to create command pool: {}", result);
    std::abort();
  }
  VkCommandBufferAllocateInfo alloc_info{};
  alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  alloc_info.commandPool = cb.command_pool;
  alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc_info.commandBufferCount = 1;
  if (const auto result =
          vkAllocateCommandBuffers(ctx.device, &alloc_info, &cb.command_buffer);
      result != VK_SUCCESS) {
    error("Failed to allocate command buffer: {}", result);
    std::abort();
  }
  return cb;
}

auto VulkanContext::transition_to_general(VkImage image,
                                          VkImageAspectFlags aspect,
                                          u32 mip_count, u32 layer_count,
                                          std::source_location loc) const
    -> void {
  one_time_submit(
      [img = image, aspect, mip_count, layer_count](VkCommandBuffer cmd) {
        const VkImageMemoryBarrier2 barrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
            .srcAccessMask = VK_ACCESS_2_NONE,
            .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .dstAccessMask =
                VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = img,
            .subresourceRange =
                {
                    .aspectMask = aspect,
                    .baseMipLevel = 0,
                    .levelCount = mip_count,
                    .baseArrayLayer = 0,
                    .layerCount = layer_count,
                },
        };

        const VkDependencyInfo dep_info{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &barrier,
        };

        vkCmdPipelineBarrier2(cmd, &dep_info);
      },
      loc);
}

auto VulkanContext::one_time_submit(std::function<void(VkCommandBuffer)> &&func,
                                    std::source_location loc) const -> void {
  VkCommandPoolCreateInfo pool_info{
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
      .queueFamilyIndex = graphics_queue_index,
  };
  VkCommandPool pool;
  vkCreateCommandPool(device, &pool_info, nullptr, &pool);

  VkCommandBufferAllocateInfo alloc_info{
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
  };
  VkCommandBuffer cmd;
  vkAllocateCommandBuffers(device, &alloc_info, &cmd);

  VkCommandBufferBeginInfo begin_info{
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };
  vkBeginCommandBuffer(cmd, &begin_info);

  func(cmd);

  vkEndCommandBuffer(cmd);

  VkCommandBufferSubmitInfo cbsi{};
  cbsi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
  cbsi.commandBuffer = cmd;
  VkSubmitInfo2 submit_info{
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
      .commandBufferInfoCount = 1,
      .pCommandBufferInfos = &cbsi,
  };

  PROFILE_SCOPE(std::format("{}", loc.function_name()));
  VkFence fence{};
  VkFenceCreateInfo fence_info{};
  fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  vkCreateFence(device, &fence_info, nullptr, &fence);
  vkResetFences(device, 1, &fence);
  vkQueueSubmit2(graphics_queue(), 1, &submit_info, fence);
  vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
  vkDestroyFence(device, fence, nullptr);

  vkDestroyCommandPool(device, pool, nullptr);
}

auto VulkanContext::one_time_submit_without_being_end(
    std::function<void(VkCommandBuffer)> &&func, std::source_location loc) const
    -> void {
  VkCommandPoolCreateInfo pool_info{
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
      .queueFamilyIndex = graphics_queue_index,
  };
  VkCommandPool pool;
  vkCreateCommandPool(device, &pool_info, nullptr, &pool);

  VkCommandBufferAllocateInfo alloc_info{
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
  };
  VkCommandBuffer cmd;
  vkAllocateCommandBuffers(device, &alloc_info, &cmd);

  func(cmd);

  VkCommandBufferSubmitInfo cbsi{};
  cbsi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
  cbsi.commandBuffer = cmd;
  VkSubmitInfo2 submit_info{
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
      .commandBufferInfoCount = 1,
      .pCommandBufferInfos = &cbsi,
  };

  PROFILE_SCOPE(std::format("One time submit: {}", loc.function_name()));
  VkFence fence{};
  VkFenceCreateInfo fence_info{};
  fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  vkCreateFence(device, &fence_info, nullptr, &fence);
  vkResetFences(device, 1, &fence);
  vkQueueSubmit2(graphics_queue(), 1, &submit_info, fence);
  vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
  vkDestroyFence(device, fence, nullptr);

  vkDestroyCommandPool(device, pool, nullptr);
}

auto VulkanContext::submit_and_wait(CommandBuffer &cb) const -> void {
  const VkCommandBufferSubmitInfo cbsi{
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
      .commandBuffer = cb.command_buffer,
  };
  const VkSubmitInfo2 submit_info{
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
      .commandBufferInfoCount = 1,
      .pCommandBufferInfos = &cbsi,
  };
  VkFenceCreateInfo fence_ci{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence fence{};
  vkCreateFence(device, &fence_ci, nullptr, &fence);
  vkQueueSubmit2(graphics_queue(), 1, &submit_info, fence);
  vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
  vkDestroyFence(device, fence, nullptr);
}

auto CommandBuffer::bind_compute(VkPipeline pipeline) -> void {
  vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
}

auto CommandBuffer::dispatch(u32 x, u32 y, u32 z) -> void {
  vkCmdDispatch(command_buffer, x, y, z);
}

auto CommandBuffer::buffer_barrier(VkBuffer buffer, VkDeviceSize offset,
                                   VkDeviceSize size,
                                   VkPipelineStageFlags2 src_stage,
                                   VkAccessFlags2 src_access,
                                   VkPipelineStageFlags2 dst_stage,
                                   VkAccessFlags2 dst_access) -> void {
  const VkBufferMemoryBarrier2 barrier{
      .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
      .srcStageMask = src_stage,
      .srcAccessMask = src_access,
      .dstStageMask = dst_stage,
      .dstAccessMask = dst_access,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .buffer = buffer,
      .offset = offset,
      .size = size,
  };
  const VkDependencyInfo dep{
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .bufferMemoryBarrierCount = 1,
      .pBufferMemoryBarriers = &barrier,
  };
  vkCmdPipelineBarrier2(command_buffer, &dep);
}

auto IblProbe::create(const VulkanContext &ctx, SceneRenderer &renderer,
                      TextureHandle equirect)
    -> std::expected<IblProbe, std::string> {
  IblProbe probe{};

  struct PushConstants {
    u32 equirect_index;
    u32 face;
    u32 mip;
    u32 max_mip;
    u32 size;
    u32 sampler_index;
    u32 out_index;
  };

  const u32 prefiltered_size = 512u;
  constexpr u32 prefiltered_mips =
      static_cast<u32>(std::bit_width(prefiltered_size));
  probe.prefiltered_mip_count = prefiltered_mips;

  auto register_tex = [&](Texture tex,
                          VkImageViewType view_type) -> TextureHandle {
    return renderer.textures.create(TextureEntry{
        .texture = std::move(tex),
        .sampled_view_type = view_type,
    });
  };

  probe.env_map = register_tex(
      Texture::create_cubemap(ctx, "ibl/env_map",
                              {
                                  .size = 512u,
                                  .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                                  .mip_levels = 1u,
                                  .storage_view = true,
                              }),
      VK_IMAGE_VIEW_TYPE_CUBE);

  probe.irradiance = register_tex(
      Texture::create_cubemap(ctx, "ibl/irradiance",
                              {
                                  .size = 64u,
                                  .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                                  .mip_levels = 1u,
                                  .storage_view = true,
                              }),
      VK_IMAGE_VIEW_TYPE_CUBE);

  probe.prefiltered = register_tex(
      Texture::create_cubemap(ctx, "ibl/prefiltered",
                              {
                                  .size = prefiltered_size,
                                  .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                                  .mip_levels = prefiltered_mips,
                                  .storage_view = true,
                              }),
      VK_IMAGE_VIEW_TYPE_CUBE);

  const u64 brdf_lut_source_hash = hash_brdf_lut_sources();
  std::optional<std::vector<std::byte>> cached_brdf_lut =
      load_cached_brdf_lut(brdf_lut_source_hash);
  if (cached_brdf_lut)
    info("IBL: loaded cached BRDF LUT (skipping regeneration)");

  auto placeholder_bytes = std::vector<std::byte>(
      usize{brdf_lut_size} * brdf_lut_size * brdf_lut_bytes_per_pixel, {});
  probe.brdf_lut = register_tex(
      Texture::from_bytes(
          ctx, "ibl/brdf_lut",
          {
              .bytes = cached_brdf_lut ? std::span(*cached_brdf_lut)
                                       : std::span(placeholder_bytes),
              .width = brdf_lut_size,
              .height = brdf_lut_size,
              .format = VK_FORMAT_R16G16_SFLOAT,
              .storage_view = true,
              .extra_usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
          }),
      VK_IMAGE_VIEW_TYPE_2D);

  auto register_cube_sub_views = [&](TextureHandle h) {
    auto &tex = renderer.textures.get(h)->texture;
    tex.register_sub_views(ctx, renderer.subimages, renderer.bindless,
                           {
                               .view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                               .layer_count = 6u,
                           });
  };
  register_cube_sub_views(probe.env_map);
  register_cube_sub_views(probe.irradiance);
  register_cube_sub_views(probe.prefiltered);

  {
    auto &tex = renderer.textures.get(probe.brdf_lut)->texture;
    tex.register_sub_views(ctx, renderer.subimages, renderer.bindless,
                           {
                               .view_type = VK_IMAGE_VIEW_TYPE_2D,
                               .layer_count = 1u,
                           });
  }

  renderer.bindless.repopulate_if_needed(renderer.textures, renderer.samplers,
                                         renderer.comparison_samplers,
                                         renderer.subimages);

  const auto desc_layout = renderer.bindless.layout;

  auto make_pipeline =
      [&](const char *path) -> std::expected<PipelineHandle, std::string> {
    auto result =
        renderer.pipeline_registry->create_compute(ComputePipelineDescription{
            .shader_path = VFSPath::create(path),
            .descriptor_set_layout = desc_layout,
            .layout = VK_NULL_HANDLE,
        });
    if (!result)
      return std::unexpected(std::format("Failed to compile pipeline '{}': {}",
                                         path, result.error()));
    return result.value();
  };

  auto pipe_equirect = make_pipeline("shaders://ibl/equirect_to_cubemap.slang");
  auto pipe_irradiance =
      make_pipeline("shaders://ibl/irradiance_convolve.slang");
  auto pipe_prefilter = make_pipeline("shaders://ibl/specular_prefilter.slang");
  auto pipe_brdf_lut = make_pipeline("shaders://ibl/brdf_lut_gen.slang");

  if (!pipe_equirect)
    return std::unexpected(pipe_equirect.error());
  if (!pipe_irradiance)
    return std::unexpected(pipe_irradiance.error());
  if (!pipe_prefilter)
    return std::unexpected(pipe_prefilter.error());
  if (!pipe_brdf_lut)
    return std::unexpected(pipe_brdf_lut.error());

  // --- barrier/dispatch helpers (unchanged) ---

  auto cube_barrier =
      [](VkImage image, u32 mip_levels, VkPipelineStageFlags2 src_stage,
         VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
         VkAccessFlags2 dst_access, VkImageLayout old_layout,
         VkImageLayout new_layout) -> VkImageMemoryBarrier2 {
    return {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .image = image,
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = mip_levels,
                .baseArrayLayer = 0,
                .layerCount = 6,
            },
    };
  };

  auto image_barrier_2d =
      [](VkImage image, VkPipelineStageFlags2 src_stage,
         VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
         VkAccessFlags2 dst_access, VkImageLayout old_layout,
         VkImageLayout new_layout) -> VkImageMemoryBarrier2 {
    return {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .image = image,
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
    };
  };

  auto submit_barriers = [](VkCommandBuffer cmd,
                            std::span<const VkImageMemoryBarrier2> barriers) {
    const VkDependencyInfo dep{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = static_cast<u32>(barriers.size()),
        .pImageMemoryBarriers = barriers.data(),
    };
    vkCmdPipelineBarrier2(cmd, &dep);
  };

  const VkDeviceSize brdf_lut_byte_size =
      VkDeviceSize{brdf_lut_size} * brdf_lut_size * brdf_lut_bytes_per_pixel;
  std::unique_ptr<Buffer> brdf_lut_readback =
      cached_brdf_lut ? nullptr
                      : Buffer::create(ctx.allocator, "brdf_lut_readback",
                                       brdf_lut_byte_size,
                                       VK_BUFFER_USAGE_TRANSFER_DST_BIT);

  const auto env_entry = renderer.textures.get(probe.env_map);
  const auto irr_entry = renderer.textures.get(probe.irradiance);
  const auto pref_entry = renderer.textures.get(probe.prefiltered);
  const auto lut_entry = renderer.textures.get(probe.brdf_lut);

  // Each IBL pass gets its own fenced submission rather than one giant batch.
  // Windows' GPU driver watchdog (TDR) kills any single uninterrupted GPU
  // submission that runs past ~2 seconds; the specular prefilter alone (1024
  // samples/texel across a 512x512x6 cubemap's full mip chain) is heavy
  // enough that bundling every pass together risked exceeding that window
  // (observed as VK_ERROR_DEVICE_LOST on startup in Release builds, where
  // there's no validation-layer overhead to inadvertently keep it under the
  // limit).
  PROFILE_SCOPE("Full compute");

  {
    PROFILE_SCOPE("IBL: equirect_to_cubemap");
    ctx.one_time_submit([&](VkCommandBuffer cmd) {
      const VkImageMemoryBarrier2 init_barriers[] = {
          cube_barrier(env_entry->texture.image, 1u, VK_PIPELINE_STAGE_2_NONE,
                       VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_IMAGE_LAYOUT_GENERAL),
          cube_barrier(irr_entry->texture.image, 1u, VK_PIPELINE_STAGE_2_NONE,
                       VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_IMAGE_LAYOUT_GENERAL),
          cube_barrier(pref_entry->texture.image, prefiltered_mips,
                       VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_IMAGE_LAYOUT_GENERAL),
          image_barrier_2d(lut_entry->texture.image, VK_PIPELINE_STAGE_2_NONE,
                           VK_ACCESS_2_NONE,
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           VK_ACCESS_2_SHADER_WRITE_BIT,
                           VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL),
      };
      submit_barriers(cmd, init_barriers);

      const auto &entry = renderer.pipeline_registry->get_entry(*pipe_equirect);
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, entry.pipeline);
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, entry.layout,
                              0u, 1u, &renderer.bindless.set, 0u, nullptr);
      const PushConstants pc{
          .equirect_index = equirect.index(),
          .size = 512u,
          .sampler_index = renderer.dummy_sampler_handle.index(),
          .out_index = env_entry->texture.sub_view_handle(0u, 0u).index(),
      };
      vkCmdPushConstants(cmd, entry.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0u,
                         sizeof(pc), &pc);
      vkCmdDispatch(cmd, 64u, 64u, 6u);

      const VkImageMemoryBarrier2 b = cube_barrier(
          env_entry->texture.image, 1u, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
          VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
          VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
          VK_IMAGE_LAYOUT_GENERAL);
      submit_barriers(cmd, {&b, 1});
    });
  }

  {
    PROFILE_SCOPE("IBL: irradiance_convolve");
    ctx.one_time_submit([&](VkCommandBuffer cmd) {
      const auto &entry =
          renderer.pipeline_registry->get_entry(*pipe_irradiance);
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, entry.pipeline);
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, entry.layout,
                              0u, 1u, &renderer.bindless.set, 0u, nullptr);
      const auto target_size = irr_entry->texture.extent.width;
      const PushConstants pc{
          .equirect_index = probe.env_map.index(),
          .size = target_size,
          .sampler_index = renderer.dummy_sampler_handle.index(),
          .out_index = irr_entry->texture.sub_view_handle(0u, 0u).index(),
      };
      vkCmdPushConstants(cmd, entry.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0u,
                         sizeof(pc), &pc);
      const auto group_count_x = (target_size + 7u) / 8u;
      const auto group_count_y = (target_size + 7u) / 8u;
      vkCmdDispatch(cmd, group_count_x, group_count_y, 6u);
    });
  }

  {
    PROFILE_SCOPE("IBL: specular_prefilter");
    const auto &entry = renderer.pipeline_registry->get_entry(*pipe_prefilter);
    for (u32 mip = 0u; mip < prefiltered_mips; ++mip) {
      ctx.one_time_submit([&](VkCommandBuffer cmd) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, entry.pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                entry.layout, 0u, 1u, &renderer.bindless.set,
                                0u, nullptr);
        const u32 mip_size = std::max(1u, prefiltered_size >> mip);
        const u32 groups = std::max(1u, mip_size / 8u);
        const PushConstants pc{
            .equirect_index = probe.env_map.index(),
            .mip = mip,
            .max_mip = prefiltered_mips - 1u,
            .size = mip_size,
            .sampler_index = renderer.dummy_sampler_handle.index(),
            .out_index = pref_entry->texture.sub_view_handle(mip, 0u).index(),
        };
        vkCmdPushConstants(cmd, entry.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0u,
                           sizeof(pc), &pc);
        vkCmdDispatch(cmd, groups, groups, 6u);
      });
    }
  }

  if (!cached_brdf_lut) {
    PROFILE_SCOPE("IBL: brdf_lut_gen");
    ctx.one_time_submit([&](VkCommandBuffer cmd) {
      const auto &entry = renderer.pipeline_registry->get_entry(*pipe_brdf_lut);
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, entry.pipeline);
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, entry.layout,
                              0u, 1u, &renderer.bindless.set, 0u, nullptr);
      const PushConstants pc{
          .size = brdf_lut_size,
          .out_index = probe.brdf_lut.index(),
      };
      vkCmdPushConstants(cmd, entry.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0u,
                         sizeof(pc), &pc);
      vkCmdDispatch(cmd, brdf_lut_size / 8u, brdf_lut_size / 8u, 1u);
    });
  }

  {
    PROFILE_SCOPE("IBL: finalize");
    ctx.one_time_submit([&](VkCommandBuffer cmd) {
      const VkImageMemoryBarrier2 final_barriers[] = {
          cube_barrier(irr_entry->texture.image, 1u,
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                       VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                       VK_IMAGE_LAYOUT_GENERAL),
          cube_barrier(pref_entry->texture.image, prefiltered_mips,
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                       VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                       VK_IMAGE_LAYOUT_GENERAL),
          image_barrier_2d(lut_entry->texture.image,
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           VK_ACCESS_2_SHADER_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                           VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                           VK_IMAGE_LAYOUT_GENERAL),
      };
      submit_barriers(cmd, final_barriers);

      if (brdf_lut_readback) {
        const VkImageMemoryBarrier2 to_transfer_read = image_barrier_2d(
            lut_entry->texture.image, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_GENERAL);
        submit_barriers(cmd, {&to_transfer_read, 1});

        const VkBufferImageCopy2 region{
            .sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
            .imageSubresource =
                {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .mipLevel = 0u,
                    .baseArrayLayer = 0u,
                    .layerCount = 1u,
                },
            .imageExtent = {.width = brdf_lut_size,
                            .height = brdf_lut_size,
                            .depth = 1u},
        };
        const VkCopyImageToBufferInfo2 copy_info{
            .sType = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2,
            .srcImage = lut_entry->texture.image,
            .srcImageLayout = VK_IMAGE_LAYOUT_GENERAL,
            .dstBuffer = brdf_lut_readback->get_buffer(),
            .regionCount = 1u,
            .pRegions = &region,
        };
        vkCmdCopyImageToBuffer2(cmd, &copy_info);
      }
    });
  }

  if (brdf_lut_readback) {
    brdf_lut_readback->invalidate();
    save_cached_brdf_lut(
        brdf_lut_source_hash,
        std::span{static_cast<const std::byte *>(
                      brdf_lut_readback->get_mapped_pointer()),
                  static_cast<usize>(brdf_lut_byte_size)});
  }

  renderer.pipeline_registry->destroy(*pipe_equirect, *pipe_irradiance,
                                      *pipe_prefilter, *pipe_brdf_lut);

  return probe;
}

auto IblProbe::destroy(const VulkanContext &ctx, SceneRenderer &renderer)
    -> void {
  auto destroy_texture = [&](TextureHandle &handle) {
    if (!handle.valid())
      return;

    if (auto *entry = renderer.textures.get(handle)) {
      entry->texture.destroy(ctx, &renderer.subimages);
      entry->texture.destroy(ctx, &renderer.textures);
    }

    renderer.textures.destroy(handle);
    handle = {};
  };

  destroy_texture(env_map);
  destroy_texture(irradiance);
  destroy_texture(prefiltered);
  destroy_texture(brdf_lut);

  prefiltered_mip_count = 0u;
}

} // namespace dy

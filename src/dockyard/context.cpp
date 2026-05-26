#include "dockyard/bindless_handle.hpp"
#include <dockyard/context.hpp>

#include <volk.h>

#include <dockyard/app.hpp>
#include <dockyard/events.hpp>
#include <dockyard/log.hpp>
#include <dockyard/pipeline_builder.hpp>
#include <dockyard/scene_renderer.hpp>
#include <dockyard/vk_check.hpp>

namespace dy {

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

  constexpr auto requested_min_images = 2;
  auto maybe_new_swapchain =
      vkb::SwapchainBuilder{ctx.physical_device, ctx.device, surface,
                            ctx.graphics_queue_index, ctx.present_queue_index}
          .set_desired_extent(width, height)
          .set_old_swapchain(swapchain)
          .use_default_format_selection()
          .set_desired_min_image_count(requested_min_images)
          .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
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
  depth_msaa =
      Texture::create(ctx, "depth_msaa", w, h, VK_FORMAT_D32_SFLOAT,
                      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                      VK_IMAGE_ASPECT_DEPTH_BIT, VK_SAMPLE_COUNT_4_BIT);

  forward_target_msaa.destroy(ctx);
  forward_target_msaa =
      Texture::create(ctx, "forward_msaa", w, h, VK_FORMAT_R16G16B16A16_SFLOAT,
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                      VK_IMAGE_ASPECT_COLOR_BIT, VK_SAMPLE_COUNT_4_BIT);

  if (forward_target.valid()) {
    auto *entry = renderer.textures.get(forward_target);
    entry->texture.destroy(ctx);
    entry->texture = Texture::create(
        ctx, "forward_target", w, h, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_STORAGE_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT);
    renderer.bindless.mark_dirty();
  } else {
    auto tex = Texture::create(
        ctx, "forward_target", w, h, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_STORAGE_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT);
    forward_target = renderer.textures.create(TextureEntry{
        .texture = std::move(tex),
        .sampled_view_type = VK_IMAGE_VIEW_TYPE_2D,
    });
  }

  // display_target: tonemapped LDR output, sampled by ImGui as a widget.
  // RGBA8 is sufficient post-tonemapping, and matches what ImGui expects to
  // display. NOTE: ensure your composite pipeline is also created with
  // VK_FORMAT_R8G8B8A8_UNORM.
  if (display_target.valid()) {
    auto *entry = renderer.textures.get(display_target);
    entry->texture.destroy(ctx);
    entry->texture = Texture::create(
        ctx, "display_target", w, h, VK_FORMAT_R8G8B8A8_SRGB,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT);
    renderer.bindless.mark_dirty();

  } else {
    auto tex = Texture::create(
        ctx, "display_target", w, h, VK_FORMAT_R8G8B8A8_SRGB,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT);
    display_target = renderer.textures.create(TextureEntry{
        .texture = tex,
        .sampled_view_type = VK_IMAGE_VIEW_TYPE_2D,
    });
  }
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

  VkPhysicalDeviceFeatures features{};
  features.multiDrawIndirect = VK_TRUE;
  features.samplerAnisotropy = VK_TRUE;
  features.textureCompressionBC = VK_TRUE;
  features.wideLines = VK_TRUE;

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

  VkPhysicalDeviceVulkan13Features features_13{};
  features_13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
  features_13.shaderDemoteToHelperInvocation = VK_TRUE;
  features_13.synchronization2 = VK_TRUE;
  features_13.dynamicRendering = VK_TRUE;
  features_13.maintenance4 = VK_TRUE;

  VkPhysicalDeviceVulkan14Features features_14{};
  features_14.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES;
  features_14.dynamicRenderingLocalRead = VK_TRUE;
  features_14.maintenance5 = VK_TRUE;
  features_14.maintenance6 = VK_TRUE;
  features_14.pushDescriptor = VK_TRUE;
  features_14.smoothLines = VK_TRUE;
  features_14.stippledSmoothLines = VK_TRUE;

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

  auto phys_ret = vkb::PhysicalDeviceSelector{ctx.instance}
                      .set_surface(ctx.surface)
                      .set_minimum_version(1, 4)
                      .set_required_features(features)
                      .set_required_features_11(features_11)
                      .set_required_features_12(features_12)
                      .set_required_features_13(features_13)
                      .set_required_features_14(features_14)
                      .add_required_extension_features(present_id_features)
                      .add_required_extension_features(present_wait_features)
                      .add_required_extensions({
                          VK_KHR_UNIFIED_IMAGE_LAYOUTS_EXTENSION_NAME,
                          VK_KHR_PRESENT_WAIT_EXTENSION_NAME,
                          VK_KHR_PRESENT_ID_EXTENSION_NAME,
                      })
                      .select();
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

  // 1. Create Timeline Semaphore
  VkSemaphoreTypeCreateInfo timeline_type_info{};
  timeline_type_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
  timeline_type_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
  timeline_type_info.initialValue = 0; // The GPU starts at 0

  VkSemaphoreCreateInfo sem_info{};
  sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  sem_info.pNext = &timeline_type_info; // Link the timeline config

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

auto IblProbe::create(const VulkanContext &ctx, SceneRenderer &renderer,
                      TextureHandle equirect) -> IblProbe {
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
  const u32 prefiltered_mips =
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

  auto bytes = std::vector<u32>(512u * 512u, {});
  probe.brdf_lut =
      register_tex(Texture::from_bytes(ctx, "ibl/brdf_lut",
                                       {
                                           .bytes = std::span(bytes),
                                           .width = 512u,
                                           .height = 512u,
                                           .format = VK_FORMAT_R16G16_SFLOAT,
                                           .storage_view = true,
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

  auto make_pipeline = [&, desc_layout =
                               renderer.bindless.layout](const VFSPath &path) {
    auto could =
        renderer.pipeline_registry->create_compute(ComputePipelineDescription{
            .shader_path = path,
            .descriptor_set_layout = desc_layout,
            .layout = VK_NULL_HANDLE,
        });
    if (!could)
      std::abort();
    return could.value();
  };

  auto compile_all = [](auto factory, auto... paths) {
    std::array pipelines{factory(VFSPath::create(paths))...};

    return std::apply(
        [](auto... elems) { return std::make_tuple(std::move(elems)...); },
        std::move(pipelines));
  };

  auto [pipe_equirect, pipe_irradiance, pipe_prefilter, pipe_brdf_lut] =
      compile_all(make_pipeline, "shaders://equirect_to_cubemap.slang",
                  "shaders://irradiance_convolve.slang",
                  "shaders://specular_prefilter.slang",
                  "shaders://brdf_lut_gen.slang");

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

  // ── Dispatch ─────────────────────────────────────────────────────────────

  PROFILE_SCOPE("Full compute");
  ctx.one_time_submit([&](VkCommandBuffer cmd) {
    PROFILE_SCOPE("Inner submission");

    const auto env_entry = renderer.textures.get(probe.env_map);
    const auto irr_entry = renderer.textures.get(probe.irradiance);
    const auto pref_entry = renderer.textures.get(probe.prefiltered);
    const auto lut_entry = renderer.textures.get(probe.brdf_lut);

    // ── [1] Transition all outputs UNDEFINED → GENERAL ───────────────────
    {
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
    }

    // ── equirect → env cubemap ────────────────────────────────────────────
    {
      const auto &entry = renderer.pipeline_registry->get_entry(pipe_equirect);
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

      // 512 / 8 = 64 groups per dim, z = 6 faces
      vkCmdDispatch(cmd, 64u, 64u, 6u);
    }

    // ── [2] env_map write → irradiance + prefilter read ───────────────────
    {
      const VkImageMemoryBarrier2 b = cube_barrier(
          env_entry->texture.image, 1u, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
          VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
          VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
          VK_IMAGE_LAYOUT_GENERAL);
      submit_barriers(cmd, {&b, 1});
    }

    // ── irradiance convolution ────────────────────────────────────────────
    {
      const auto &entry =
          renderer.pipeline_registry->get_entry(pipe_irradiance);
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, entry.pipeline);
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, entry.layout,
                              0u, 1u, &renderer.bindless.set, 0u, nullptr);

      const PushConstants pc{
          .equirect_index = probe.env_map.index(),
          .size = 32u,
          .sampler_index = renderer.dummy_sampler_handle.index(),
          .out_index = irr_entry->texture.sub_view_handle(0u, 0u).index(),
      };
      vkCmdPushConstants(cmd, entry.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0u,
                         sizeof(pc), &pc);

      // 32 / 8 = 4 groups per dim, z = 6 faces
      vkCmdDispatch(cmd, 4u, 4u, 6u);
    }

    // ── specular prefilter — one dispatch per mip ─────────────────────────
    {
      const auto &entry = renderer.pipeline_registry->get_entry(pipe_prefilter);
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, entry.pipeline);
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, entry.layout,
                              0u, 1u, &renderer.bindless.set, 0u, nullptr);

      for (u32 mip = 0u; mip < prefiltered_mips; ++mip) {
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
      }
    }

    // ── BRDF LUT ──────────────────────────────────────────────────────────
    {
      const auto &entry = renderer.pipeline_registry->get_entry(pipe_brdf_lut);
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, entry.pipeline);
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, entry.layout,
                              0u, 1u, &renderer.bindless.set, 0u, nullptr);

      const PushConstants pc{
          .size = 512u,
          .out_index = probe.brdf_lut.index(),
      };
      vkCmdPushConstants(cmd, entry.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0u,
                         sizeof(pc), &pc);

      // 512 / 8 = 64 groups per dim
      vkCmdDispatch(cmd, 64u, 64u, 1u);
    }

    // ── [3] Final barrier: compute writes → fragment shader reads ─────────
    {
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
    }
  });

  renderer.pipeline_registry->destroy(pipe_equirect, pipe_irradiance,
                                      pipe_prefilter, pipe_brdf_lut);

  return probe;
}

auto IblProbe::destroy(const VulkanContext &ctx, SceneRenderer &renderer)
    -> void {
  if (auto *e = renderer.textures.get(prefiltered))
    e->texture.destroy(ctx, &renderer.subimages);

  for (auto h : {env_map, irradiance, brdf_lut}) {
    if (auto *e = renderer.textures.get(h))
      e->texture.destroy(ctx);
    renderer.textures.destroy(h);
  }
  renderer.textures.destroy(prefiltered);

  *this = {};
}

} // namespace dy

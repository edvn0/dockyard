#include "dockyard/events.hpp"
#include <dockyard/context.hpp>

#include <volk.h>

#include <dockyard/app.hpp>
#include <dockyard/log.hpp>
#include <dockyard/vk_check.hpp>
#include <vulkan/vulkan_core.h>

namespace dy {

auto RendererListener::on_swapchain_invalidated(
    const events::swapchain_invalidated &e) const -> void {

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

  dispatch.trigger<events::swapchain_resized>(
      events::swapchain_resized{e.width, e.height});
}

auto RendererListener::on_window_minimized(const events::window_minimized &e)
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
  for (auto view : image_views)
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

  info("Swapchain rebuilt: {}x{}, image count: {}", width, height,
       swapchain.image_count);
}

auto SwapchainResources::destroy(const VulkanContext &ctx) -> void {
  for (auto view : image_views)
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
auto ViewportResources::resize(const VulkanContext &ctx, u32 width, u32 height)
    -> void {
  depth_msaa.destroy(ctx);
  forward_target.destroy(ctx);

  depth_msaa =
      RenderTarget::create(ctx, width, height, VK_FORMAT_D32_SFLOAT,
                           VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                           VK_IMAGE_ASPECT_DEPTH_BIT, VK_SAMPLE_COUNT_4_BIT);

  forward_target = RenderTarget::create(
      ctx, width, height, VK_FORMAT_R16G16B16A16_SFLOAT,
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
          VK_IMAGE_USAGE_STORAGE_BIT,
      VK_IMAGE_ASPECT_COLOR_BIT);
  forward_target_msaa =
      RenderTarget::create(ctx, width, height, VK_FORMAT_R16G16B16A16_SFLOAT,
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT, VK_SAMPLE_COUNT_4_BIT);
}
auto ViewportResources::destroy(const VulkanContext &ctx) -> void {
  depth_msaa.destroy(ctx);
  forward_target.destroy(ctx);
  forward_target_msaa.destroy(ctx);
}
auto ViewportResources::create(const VulkanContext &ctx, u32 width, u32 height)
    -> ViewportResources {
  ViewportResources r{};
  r.resize(ctx, width, height);
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

  ctx.surface = std::move(s);
  ctx.instance = std::move(inst);
  volkLoadInstance(ctx.instance.instance);

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

  VkPhysicalDevicePresentIdFeaturesKHR present_id_features{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR,
      .pNext = nullptr,
      .presentId = VK_TRUE};

  VkPhysicalDevicePresentWaitFeaturesKHR present_wait_features{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR,
      .pNext = nullptr,
      .presentWait = VK_TRUE};

  auto phys_ret = vkb::PhysicalDeviceSelector{ctx.instance}
                      .set_surface(ctx.surface)
                      .set_minimum_version(1, 4)
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

auto RenderTarget::valid() const -> bool { return image != VK_NULL_HANDLE; }
auto RenderTarget::destroy(const VulkanContext &ctx) -> void {
  if (!valid())
    return;
  vkDestroyImageView(ctx.device, view, nullptr);
  vmaDestroyImage(ctx.allocator, image, allocation);
  *this = {};
}

auto RenderTarget::create(const VulkanContext &ctx, u32 width, u32 height,
                          VkFormat format, VkImageUsageFlags usage,
                          VkImageAspectFlags aspect,
                          VkSampleCountFlagBits samples) -> RenderTarget {
  RenderTarget rt{};
  rt.format = format;
  rt.extent = {width, height};

  VkImageCreateInfo image_info{};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = format;
  image_info.extent = {width, height, 1};
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

  if (image_info.usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) {
    vma_info.preferredFlags = VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT;
  }

  if (const auto result =
          vmaCreateImage(ctx.allocator, &image_info, &vma_info, &rt.image,
                         &rt.allocation, &rt.allocation_info);
      result != VK_SUCCESS) {
    error("Failed to create render target image: {}", result);
    std::abort();
  }

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
          vkCreateImageView(ctx.device, &view_info, nullptr, &rt.view);
      result != VK_SUCCESS) {
    std::abort();
  }

  return rt;
}

} // namespace dy

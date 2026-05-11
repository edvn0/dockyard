#pragma once

#include <dockyard/bindless_descriptor.hpp>
#include <dockyard/events.hpp>
#include <dockyard/texture.hpp>
#include <dockyard/types.hpp>

#include <volk.h>

#include <VkBootstrap.h>
#include <entt/entt.hpp>
#include <vk_mem_alloc.h>

namespace dy {

struct SceneRenderer;

struct VulkanContext {
  vkb::Instance instance{};
  vkb::PhysicalDevice physical_device{};
  vkb::Device device{};
  VkSurfaceKHR surface{};
  VmaAllocator allocator{};
  u32 graphics_queue_index{};
  u32 present_queue_index{};

  auto graphics_queue() const -> VkQueue;
  auto present_queue() const -> VkQueue;
  auto destroy() -> void;
  static auto create(vkb::Instance &&inst, VkSurfaceKHR &&s) -> VulkanContext;
};
struct FrameSync {
  VkSemaphore timeline_semaphore{};        // Timeline
  u64 last_value{0};                       // Current GPU progress
  VkFence in_flight_fence{};               // Legacy backup / non-present sync
  VkSemaphore image_available_semaphore{}; // Binary (Acquire)

  auto destroy(const VulkanContext &ctx) -> void;
  static auto create(const VulkanContext &ctx) -> FrameSync;
};
struct ImageSync {
  VkSemaphore render_finished_semaphore{}; // Binary (Release)

  auto destroy(const VulkanContext &ctx) -> void;
  static auto create(const VulkanContext &ctx) -> ImageSync;
};
struct SwapchainImageView {
  VkImage image;
  VkImageView view;
  VkExtent2D extent;
};
struct RenderContext {
  VkCommandBuffer main_cb; // Pre-started for convenience

  SwapchainImageView swapchain_image;
  u64 frame_index;

  u64 wait_value;              // Start of this frame's GPU timeline
  VkSemaphore image_available; // Wait on this for the FIRST swapchain touch
  VkSemaphore render_finished; // Signal this in the LAST submit

  auto next_frame_wait_value() const { return wait_value + 1; }
};
struct CommandBuffer {
  VkCommandPool command_pool{};
  VkCommandBuffer command_buffer{};

  auto reset(const VulkanContext &ctx) -> void;
  auto begin(VkCommandBufferUsageFlags =
                 VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT) -> bool;
  auto end() -> bool;
  auto destroy(const VulkanContext &ctx) -> void;
  static auto create(const VulkanContext &ctx) -> CommandBuffer;
};

struct ViewportResources {
  Texture depth_msaa{};
  Texture forward_target_msaa{};

  TextureHandle forward_target{};
  TextureHandle display_target{};

  auto extent() const { return forward_target_msaa.extent; }

  auto resize(const VulkanContext &ctx, SceneRenderer &, u32 width, u32 height)
      -> void;
  auto destroy(const VulkanContext &ctx) -> void;
  static auto create(const VulkanContext &ctx, SceneRenderer &, u32 width,
                     u32 height) -> ViewportResources;
};
struct SwapchainResources {
  vkb::Swapchain swapchain{};
  std::vector<VkImage> images;
  std::vector<VkImageView> image_views;
  std::vector<ImageSync> image_sync;

  auto extent() const { return swapchain.extent; }
  auto format() const { return swapchain.image_format; }

  auto rebuild(const VulkanContext &ctx, VkSurfaceKHR surface, u32 width,
               u32 height) -> void;

  auto destroy(const VulkanContext &ctx) -> void;

  static auto create(const VulkanContext &ctx, VkSurfaceKHR surface, u32 width,
                     u32 height) -> SwapchainResources;
};
struct FrameResources {
  std::vector<CommandBuffer> command_buffers;
  std::vector<FrameSync> frame_sync;

  auto destroy(const VulkanContext &ctx) -> void;
  static auto create(const VulkanContext &ctx) -> FrameResources;
};
class App;
struct RendererListener {
  const VulkanContext &ctx;
  SwapchainResources &sc;
  VkSurfaceKHR surface{};
  FrameResources &frames;
  u64 &frame_index;
  std::vector<u64> &frame_ids;
  entt::dispatcher &dispatch;
  mutable bool minimized{false};
  mutable bool needs_recreation{false};

  auto on_swapchain_invalidated(const events::swapchain_invalidated &e) const
      -> void;
  auto on_window_minimized(const events::window_minimized &e) -> void;
};

} // namespace dy
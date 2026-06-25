#pragma once

#include "dockyard/bindless_handle.hpp"
#include <dockyard/bindless_descriptor.hpp>
#include <dockyard/events.hpp>
#include <dockyard/texture.hpp>
#include <dockyard/types.hpp>

#include <source_location>
#include <volk.h>

#include <VkBootstrap.h>
#include <vk_mem_alloc.h>
#include <entt/fwd.hpp>

namespace dy {

struct SceneRenderer;
struct CommandBuffer;

struct VulkanContext {
  vkb::Instance instance{};
  vkb::PhysicalDevice physical_device{};
  vkb::Device device{};
  VkSurfaceKHR surface{};
  VmaAllocator allocator{};
  u32 graphics_queue_index{};
  u32 present_queue_index{};

  struct Caps {
    bool unified_image_layouts = false;
    bool present_wait = false;
    bool maintenance5 = false;
    bool maintenance6 = false;
    bool smooth_lines = false;
    bool stippled_smooth_lines = false;
    bool executable_properties = false;
    bool push_descriptor = false; // promoted in 1.4, but guard anyway
    bool transient_attachments =
        false; // lazily-allocated memory exists (mobile/tiled GPUs)
  } caps;

  auto graphics_queue() const -> VkQueue;
  auto present_queue() const -> VkQueue;
  auto destroy() -> void;

  auto
  one_time_submit(std::function<void(VkCommandBuffer)> &&func,
                  std::source_location = std::source_location::current()) const
      -> void;
  auto one_time_submit_without_being_end(
      std::function<void(VkCommandBuffer)> &&func,
      std::source_location = std::source_location::current()) const -> void;
  auto transition_to_general(
      VkImage, VkImageAspectFlags aspect, u32 mip_count, u32 layer_count,
      std::source_location = std::source_location::current()) const -> void;

  // Submit a fully-recorded CommandBuffer synchronously (blocks until done).
  auto submit_and_wait(CommandBuffer &cb) const -> void;

  static auto create(vkb::Instance &&inst, VkSurfaceKHR &&s) -> VulkanContext;
};
struct FrameSync {
  VkSemaphore timeline_semaphore{};        // Timeline
  u64 last_value{0};                       // Current GPU progress
  VkFence in_flight_fence{};               // Legacy backup / non-present sync
  VkSemaphore image_available_semaphore{}; // Binary (Acquire)
  u64 past_presentation_id = 0;

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
  u64 last_frame_index;

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

  auto bind_compute(VkPipeline pipeline) -> void;

  template <typename T>
  auto push_constants(VkPipelineLayout layout, VkShaderStageFlags stages,
                      u32 offset, const T &data) -> void {
    vkCmdPushConstants(command_buffer, layout, stages, offset, sizeof(T), &data);
  }

  auto dispatch(u32 x, u32 y, u32 z) -> void;

  auto buffer_barrier(VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size,
                      VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                      VkPipelineStageFlags2 dst_stage,
                      VkAccessFlags2 dst_access) -> void;
};

struct ViewportResources {
  Texture depth_msaa{};
  Texture forward_target_msaa{};

  TextureHandle forward_target{};
  TextureHandle depth_resolved_target{};
  TextureHandle depth_pre_hiz{};
  TextureHandle display_target{};

  TextureHandle hierarchical_depth_pyramid_target{};

  [[nodiscard]] auto extent() const { return forward_target_msaa.extent; }

  auto resize(const VulkanContext &ctx, SceneRenderer &, u32 width, u32 height)
      -> void;
  auto destroy(const VulkanContext &ctx) -> void;
  static auto create(const VulkanContext &ctx, SceneRenderer &, u32 width,
                     u32 height) -> ViewportResources;
};

struct IblProbe {
  TextureHandle env_map;     // raw equirect → cubemap (512³, 1 mip)
  TextureHandle irradiance;  // diffuse integral (32³, 1 mip)
  TextureHandle prefiltered; // specular mip-chain (256³, N mips)
  TextureHandle brdf_lut;    // 2D RG16F (512×512)

  u32 prefiltered_mip_count{};

  [[nodiscard]] auto valid() const {
    return env_map.valid() && irradiance.valid() && prefiltered.valid() &&
           brdf_lut.valid();
  }

  static auto create(const VulkanContext &, SceneRenderer &, TextureHandle)
      -> std::expected<IblProbe, std::string>;
  auto destroy(const VulkanContext &, SceneRenderer &) -> void;
};

struct SwapchainResources {
  vkb::Swapchain swapchain{};
  std::vector<VkImage> images;
  std::vector<VkImageView> image_views;
  std::vector<ImageSync> image_sync;

  [[nodiscard]] auto extent() const { return swapchain.extent; }
  [[nodiscard]] auto format() const { return swapchain.image_format; }

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

  auto on_swapchain_invalidated(const events::SwapchainInvalidated &e) const
      -> void;
  auto on_window_minimized(const events::WindowMinimized &e) -> void;
};

} // namespace dy
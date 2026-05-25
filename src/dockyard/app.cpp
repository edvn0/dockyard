#include <csignal>
#include <dockyard/app.hpp>
#include <dockyard/context.hpp>
#include <dockyard/event_callbacks.hpp>
#include <dockyard/imgui_renderer.hpp>
#include <dockyard/log.hpp>
#include <dockyard/renderdoc.hpp>
#include <dockyard/scope_exit.hpp>
#include <dockyard/vfs.hpp>
#include <dockyard/vk_check.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <ranges>
#include <span>
#include <vector>
#include <volk.h>

#include <GLFW/glfw3.h>
#include <VkBootstrap.h>
#include <entt/entt.hpp>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan_core.h>

struct TimeStep {
  double last_time{glfwGetTime()};
  float step() {
    const double current_time = glfwGetTime();
    const auto delta_time = static_cast<float>(current_time - last_time);
    last_time = current_time;
    return delta_time;
  }
};

namespace dy {

auto acquire_swapchain_image(const SwapchainResources &, const FrameSync &)
    -> std::optional<u32>;
auto submit_to_queue(const VulkanContext &, VkCommandBuffer, const FrameSync &,
                     const ImageSync &, u64) -> bool;
auto glfw_error_logger() -> void;

std::atomic_bool running{true};
static auto handler(auto) -> void {
  warn("Interrupted - exiting");
  running = false;
}

auto App::run(i32 argc, char *argv[]) -> i32 {
  std::signal(SIGINT, handler);

#ifdef ASSETS_ROOT_PATH
  VFS::get().initialize(ASSETS_ROOT_PATH);
#else
  VFS::get().initialize(std::filesystem::current_path() / "assets");
#endif

  auto renderdoc = renderdoc_init();
  if (glfwPlatformSupported(GLFW_PLATFORM_WAYLAND) == GLFW_TRUE) {
    glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_WAYLAND);
  }
  if (renderdoc.is_active()) {
    glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
    info("RenderDoc prefers X11.");
  }

  auto parameters =
      std::span{argv, static_cast<usize>(argc)} | std::views::drop(1) |
      std::views::transform([](auto arg) { return std::string_view{arg}; });
  auto find_or_default = [&p = parameters](std::string_view prefix,
                                           int default_val) -> int {
    auto it = std::ranges::find_if(
        p, [prefix](auto arg) { return arg.starts_with(prefix); });

    if (it == p.end())
      return default_val;

    std::string_view value_str = *it;
    value_str.remove_prefix(prefix.size());

    int result{};
    auto [ptr, ec] = std::from_chars(
        value_str.data(), value_str.data() + value_str.size(), result);

    return (ec == std::errc{}) ? result : default_val;
  };

  if (glfwInit() != GLFW_TRUE) {
    glfw_error_logger();
    return -1;
  }
  DEFER(glfwTerminate());

  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

  int width = find_or_default("--width=", 1280);
  int height = find_or_default("--height=", 720);
  window = glfwCreateWindow(width, height, "Dockyard", nullptr, nullptr);
  if (!window) {
    glfw_error_logger();
    return -1;
  }
  DEFER(glfwDestroyWindow(window));
  glfwHideWindow(window);

  entt::dispatcher dispatcher{};
  install_glfw_callbacks(window, dispatcher);
  DEFER(uninstall_glfw_callbacks(window));

  vk::check(volkInitialize());
  DEFER(volkFinalize());

  auto precache_future = shader::Compiler::the().precache_shaders({});
  precache_future.wait();

  auto inst_ret =
      vkb::InstanceBuilder{}
          .set_app_name("Dockyard")
          .set_engine_name("Dockyard Engine")
          .set_app_version(0, 1, 0)
          .set_engine_version(0, 1, 0)
          .require_api_version(1, 4, 0)
          .enable_validation_layers(enable_validation_layers)
          .request_validation_layers(enable_validation_layers)
          .set_debug_callback(
              [](VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                 VkDebugUtilsMessageTypeFlagsEXT,
                 const VkDebugUtilsMessengerCallbackDataEXT *data,
                 void *) -> VkBool32 {
                std::string object_names;
                if (data->objectCount > 0) {
                  object_names = " | Objects: ";
                  for (u32 i = 0; i < data->objectCount; ++i) {
                    // pObjectName may be null if the application didn't set it
                    const char *name = data->pObjects[i].pObjectName;
                    object_names += (name ? name : "<unnamed>");
                    if (i < data->objectCount - 1)
                      object_names += ", ";
                  }
                }

                std::string full_msg =
                    std::string(data->pMessage) + object_names;

                if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
                  error("Vulkan: {}", full_msg);
                else if (severity &
                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
                  warn("Vulkan: {}", full_msg);
                else
                  info("Vulkan: {}", full_msg);

                return VK_FALSE;
              })
          .build();
  if (!inst_ret) {
    error("Failed to create instance: {}", inst_ret.error().message());
    std::abort();
  }
  auto instance = inst_ret.value();

  VkSurfaceKHR surface;
  if (glfwCreateWindowSurface(instance.instance, window, nullptr, &surface) !=
      VK_SUCCESS) {
    return -1;
  }

  // VulkanContext takes ownership of the instance and the surface and destroys
  // them internally.
  auto ctx = VulkanContext::create(std::move(instance), std::move(surface));
  DEFER(ctx.destroy());

  u64 frame_id = 1;
  auto sc = SwapchainResources::create(ctx, surface, static_cast<u32>(width),
                                       static_cast<u32>(height));
  auto frames = FrameResources::create(ctx);
  DEFER(sc.destroy(ctx));
  DEFER(frames.destroy(ctx));
  std::vector<u64> image_last_frame_id(sc.images.size(), 0);

  RendererListener render_listener{
      .ctx = ctx,
      .sc = sc,
      .surface = surface,
      .frames = frames,
      .frame_index = frame_index,
      .frame_ids = image_last_frame_id,
      .dispatch = dispatcher,
  };
  dispatcher.sink<events::WindowMinimized>()
      .connect<&RendererListener::on_window_minimized>(render_listener);
  dispatcher.sink<events::SwapchainInvalidated>()
      .connect<&RendererListener::on_swapchain_invalidated>(render_listener);
  dispatcher.sink<events::SwapchainResized>()
      .connect<&App::on_swapchain_resized>(*this);
  dispatcher.sink<events::KeyPressed>().connect<&App::on_key_pressed>(*this);
  dispatcher.sink<events::KeyReleased>().connect<&App::on_key_released>(*this);
  dispatcher.sink<events::MouseButtonPressed>()
      .connect<&App::on_mouse_button_pressed>(*this);
  dispatcher.sink<events::MouseButtonReleased>()
      .connect<&App::on_mouse_button_released>(*this);
  dispatcher.sink<events::MouseMoved>().connect<&App::on_mouse_moved>(*this);
  dispatcher.sink<events::MouseScrolled>().connect<&App::on_mouse_scrolled>(
      *this);

  {
    InitialisationContext init_context{
        .window_extent = {sc.swapchain.extent.width,
                          sc.swapchain.extent.height},
        .viewport_extent = {sc.swapchain.extent.width,
                            sc.swapchain.extent.height},
        .context = ctx,
        .swapchain_resources = sc,
    };
    init(init_context);
  }

  glfwShowWindow(window);

  TimeStep ts{};
  while (running && (glfwWindowShouldClose(window) != VK_TRUE)) {
    glfwPollEvents();
    poll_pending_events();
    dispatcher.update();

    update(ts.step());

    if (render_listener.minimized)
      continue;

    if (render_listener.needs_recreation) {
      App::recreate_swapchain_manually(window, render_listener);
      continue;
    }

    auto &frame = frames.frame_sync[frame_index];
    vkWaitForFences(ctx.device, 1, &frame.in_flight_fence, VK_TRUE, UINT64_MAX);
    DeletionQueue::the().begin_frame(frame_index);

    auto maybe_index = acquire_swapchain_image(sc, frame);
    if (!maybe_index) {
      App::recreate_swapchain_manually(window, render_listener);
      continue;
    }
    auto index = std::move(maybe_index).value();

    if (const auto last_use = image_last_frame_id[index]; last_use > 0) {
      constexpr auto delay =
          std::chrono::nanoseconds(std::chrono::milliseconds(16)).count();
      vkWaitForPresentKHR(ctx.device, sc.swapchain.swapchain, last_use, delay);
    }

    auto &image = sc.image_sync[index];
    auto &cb = frames.command_buffers[frame_index];
    cb.reset(ctx);

    RenderContext r_ctx{
        .main_cb = cb.command_buffer,
        .swapchain_image =
            SwapchainImageView{
                .image = sc.images[index],
                .view = sc.image_views[index],
                .extent = sc.swapchain.extent,
            },
        .frame_index = frame_index,
        .last_frame_index = last_frame_index,
        .wait_value = frame.last_value,
        .image_available = frame.image_available_semaphore,
        .render_finished = image.render_finished_semaphore,
    };

    if (!cb.begin())
      return -1;

    u64 end_val = render(r_ctx);
    frame.last_value = end_val;

    if (!cb.end())
      return -1;

    vkResetFences(ctx.device, 1, &frame.in_flight_fence);
    if (!submit_to_queue(ctx, cb.command_buffer, frame, image, end_val))
      return -1;

    VkPresentIdKHR present_id_info{
        .sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR,
        .pNext = nullptr,
        .swapchainCount = 1,
        .pPresentIds = &frame_id,
    };

    VkPresentInfoKHR present_info{};
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.pNext = &present_id_info;
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = &image.render_finished_semaphore;
    present_info.swapchainCount = 1;
    present_info.pSwapchains = &sc.swapchain.swapchain;
    present_info.pImageIndices = &index;

    const auto present_result =
        vkQueuePresentKHR(ctx.present_queue(), &present_info);
    if (present_result == VK_ERROR_OUT_OF_DATE_KHR ||
        present_result == VK_SUBOPTIMAL_KHR) {
      App::recreate_swapchain_manually(window, render_listener);
    } else if (present_result != VK_SUCCESS) {
      return -1;
    }

    image_last_frame_id[index] = frame_id;
    frame_id++;
    last_frame_index = frame_index;
    frame_index = (frame_index + 1) % frames_in_flight;
  }

  vkDeviceWaitIdle(ctx.device);
  destroy();
  vkDeviceWaitIdle(ctx.device);
  DeletionQueue::the().flush_all();

  info("Application exited successfully");
  return 0;
}

auto App::on_swapchain_resized(const events::SwapchainResized &e) -> void {
  resize(e.width, e.height);
}

auto App::recreate_swapchain_manually(GLFWwindow *window,
                                      const RendererListener &render_listener)
    -> void {
  int w{};
  int h{};
  glfwGetFramebufferSize(window, &w, &h);

  if (w == 0 || h == 0)
    return;

  render_listener.on_swapchain_invalidated({
      .width = static_cast<u32>(w),
      .height = static_cast<u32>(h),
  });
}

auto acquire_swapchain_image(const SwapchainResources &sc,
                             const FrameSync &frame) -> std::optional<u32> {
  u32 index{};
  const auto result = vkAcquireNextImageKHR(
      sc.swapchain.device, sc.swapchain.swapchain, UINT64_MAX,
      frame.image_available_semaphore, VK_NULL_HANDLE, &index);
  if (result == VK_ERROR_OUT_OF_DATE_KHR)
    return std::nullopt;
  if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
    error("Failed to acquire swapchain image: {}", result);
    return std::nullopt;
  }
  return {index};
}

auto submit_to_queue(const VulkanContext &ctx, VkCommandBuffer command_buffer,
                     const FrameSync &frame, const ImageSync &image,
                     u64 signal_val) -> bool {

  VkSemaphoreSubmitInfo wait_info{};
  wait_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
  wait_info.semaphore = frame.image_available_semaphore;
  wait_info.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

  VkSemaphoreSubmitInfo binary_signal{};
  binary_signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
  binary_signal.semaphore = image.render_finished_semaphore;
  binary_signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

  VkSemaphoreSubmitInfo timeline_signal{};
  timeline_signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
  timeline_signal.semaphore = frame.timeline_semaphore;
  timeline_signal.value = signal_val;
  timeline_signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

  const std::array signals = {
      binary_signal,
      timeline_signal,
  };

  VkCommandBufferSubmitInfo cb_submit_info{};
  cb_submit_info.pNext = nullptr;
  cb_submit_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
  cb_submit_info.commandBuffer = command_buffer;

  VkSubmitInfo2 submit_info{};
  submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
  submit_info.waitSemaphoreInfoCount = 1;
  submit_info.pWaitSemaphoreInfos = &wait_info;
  submit_info.commandBufferInfoCount = 1;
  submit_info.pCommandBufferInfos = &cb_submit_info;
  submit_info.signalSemaphoreInfoCount = std::size(signals);
  submit_info.pSignalSemaphoreInfos = signals.data();

  return vkQueueSubmit2(ctx.graphics_queue(), 1, &submit_info,
                        frame.in_flight_fence) == VK_SUCCESS;
}

auto glfw_error_logger() -> void {
  const char *description;
  glfwGetError(&description);
  error("GLFW error: {}", description);
}

auto DeletionQueue::the() -> DeletionQueue & {
  static DeletionQueue instance;
  return instance;
}

auto DeletionQueue::push(Fn &&fn) -> void {
  per_frame[current_frame].push_back(std::move(fn));
}

auto DeletionQueue::on_destroy(Fn &&fn) -> void {
  on_app_exit.push_back(std::move(fn));
}

auto DeletionQueue::destroy_at_exit(VkDevice device, VkPipeline pipeline,
                                    VkPipelineLayout pipeline_layout) -> void {
  on_app_exit.emplace_back([device, pipeline, pipeline_layout] {
    if (pipeline != VK_NULL_HANDLE)
      vkDestroyPipeline(device, pipeline, nullptr);
    if (pipeline_layout != VK_NULL_HANDLE)
      vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
  });
}

auto DeletionQueue::flush_all() -> void {
  std::ranges::for_each(per_frame, [](auto &v) {
    std::ranges::for_each(v, [](auto &k) { k(); });
  });
  std::ranges::for_each(on_app_exit, [](auto &fn) { fn(); });
}

auto DeletionQueue::flush(u32 frame_index) -> void {
  auto &functions = per_frame[frame_index];
  for (auto &fn : functions)
    fn();
  functions.clear();
}

} // namespace dy
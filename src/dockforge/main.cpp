#include <volk.h>

#include <imgui.h>

#include <GLFW/glfw3.h>
#include <ImGuizmo.h>
#include <algorithm>
#include <cassert>
#include <concepts>
#include <entt/entt.hpp>
#include <expected>
#include <fstream>
#include <glm/packing.hpp>
#include <memory>
#include <numeric>
#include <ranges>
#include <utility>

#include <dockyard/app.hpp>
#include <dockyard/bindless_descriptor.hpp>
#include <dockyard/buffer.hpp>
#include <dockyard/check_operation.hpp>
#include <dockyard/compiler.hpp>
#include <dockyard/context.hpp>
#include <dockyard/device_geometry.hpp>
#include <dockyard/events.hpp>
#include <dockyard/imgui_renderer.hpp>
#include <dockyard/pipeline_builder.hpp>
#include <dockyard/scene.hpp>
#include <dockyard/scene_renderer.hpp>
#include <dockyard/texture.hpp>
#include <dockyard/types.hpp>
#include <dockyard/vfs.hpp>
#include <dockyard/vfs_path.hpp>
#include <dockyard/vk_check.hpp>
#include <vulkan/vulkan_core.h>

using namespace dy;

struct FreeCameraController {
  Components::Camera &camera;
  GLFWwindow *window = nullptr;
  float move_speed = 5.0f;
  float sensitivity = 0.1f;

  explicit FreeCameraController(Components::Camera &cam, GLFWwindow *w)
      : camera(cam), window(w) {}

  auto update(float ts) -> void {
    auto fwd = camera.forward();
    auto right = camera.right();
    constexpr glm::vec3 world_up{0.0f, 1.0f, 0.0f};

    glm::vec3 dir{0.0f};
    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS)
      dir += fwd;
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS)
      dir -= fwd;
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS)
      dir += right;
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS)
      dir -= right;
    if (glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS)
      dir += world_up;
    if (glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS)
      dir -= world_up;

    if (glm::length(dir) > 1e-6f)
      camera.position += glm::normalize(dir) * move_speed * ts;
  }

  auto on_mouse_delta(float dx, float dy) -> void {
    camera.yaw += glm::radians(dx * sensitivity);
    camera.pitch = glm::clamp(camera.pitch - glm::radians(dy * sensitivity),
                              glm::radians(-89.0f), glm::radians(89.0f));
  }
};

enum class EaseType { Linear, SmoothStep, InOutSine };

struct CameraKeyframe {
  glm::vec3 position;
  glm::quat orientation;
  float travel_time;
  EaseType easing = EaseType::SmoothStep;
};

struct PathCameraController {
  Components::Camera &camera;
  std::vector<CameraKeyframe> path;

  usize current_index = 0;
  float segment_time = 0.0f;
  bool is_playing = false;

  float apply_easing(float t, EaseType type) {
    switch (type) {
    case EaseType::SmoothStep:
      return t * t * (3.0f - 2.0f * t);
    case EaseType::InOutSine:
      return -(glm::cos(glm::pi<float>() * t) - 1.0f) / 2.0f;
    default:
      return t;
    }
  }

  void update(float ts) {
    if (!is_playing || current_index >= path.size() - 1) {
      camera.forward_override = std::nullopt;
      return;
    }

    const auto &start = path[current_index];
    const auto &end = path[current_index + 1];

    segment_time += ts;
    float linear_t = glm::clamp(segment_time / end.travel_time, 0.0f, 1.0f);
    float eased_t = apply_easing(linear_t, end.easing);

    camera.position = glm::mix(start.position, end.position, eased_t);

    glm::quat current_rot =
        glm::slerp(start.orientation, end.orientation, eased_t);

    glm::vec3 path_forward = current_rot * glm::vec3(0.0f, 0.0f, 1.0f);
    camera.forward_override = glm::normalize(path_forward);

    if (linear_t >= 1.0f) {
      current_index++;
      segment_time = 0.0f;

      if (current_index >= path.size() - 1) {
        auto [y, p] = Components::Camera::facing_toward(
            glm::vec3(0), *camera.forward_override);
        camera.yaw = y;
        camera.pitch = p;
        camera.forward_override = std::nullopt;
        is_playing = false;
      }
    }
  }

  void update_camera_from_quat(const glm::quat &q) {
    glm::vec3 forward = q * glm::vec3(0, 0, -1);
    auto [y, p] = Components::Camera::facing_toward(glm::vec3(0.0f), forward);
    camera.yaw = y;
    camera.pitch = p;
  }
};

struct EditorCamera {
  EditorCamera(const EditorCamera &) = delete;
  EditorCamera &operator=(const EditorCamera &) = delete;
  EditorCamera(EditorCamera &&) = delete;
  EditorCamera &operator=(EditorCamera &&) = delete;

  Components::Camera camera{};
  FreeCameraController free_controller;
  PathCameraController path_controller;

  bool use_path = false;

  EditorCamera(GLFWwindow *w, glm::vec3 position, glm::vec3 look_at, u32 width,
               u32 height)
      : free_controller(camera, w), path_controller(camera) {
    camera.position = position;
    camera.set_aspect(width, height);
    auto [y, p] = Components::Camera::facing_toward(position, look_at);
    camera.yaw = y;
    camera.pitch = p;
  }

  auto save_keyframe(float time_to_reach) -> void {
    glm::vec3 fwd = camera.forward();
    glm::quat rot = glm::quatLookAtLH(fwd, glm::vec3(0, 1, 0));
    path_controller.path.push_back(
        {camera.position, rot, time_to_reach, EaseType::SmoothStep});
  }

  auto update(float ts) -> void {
    if (use_path)
      path_controller.update(ts);
    else
      free_controller.update(ts);
  }

  auto on_mouse_delta(float dx, float dy) -> void {
    if (!use_path)
      free_controller.on_mouse_delta(dx, dy);
  }

  auto set_aspect(u32 w, u32 h) -> void { camera.set_aspect(w, h); }
  [[nodiscard]] auto view() const -> glm::mat4 { return camera.view(); }
  [[nodiscard]] auto projection() const -> glm::mat4 {
    return camera.projection();
  }
};

#include "./cube_vertices.inl"

constexpr auto operator==(const VkExtent2D &l, const VkExtent2D &r) -> bool {
  return l.width == r.width && l.height == r.height;
}

struct Dockforge : App {
  std::shared_ptr<Scene> editor_scene;
  std::shared_ptr<Scene> runtime_scene;
  std::shared_ptr<Scene> active_scene;

  GLFWwindow *main_window = nullptr;

  Mesh mesh_handle{};
  PipelineHandle forward_pipeline{};
  PipelineHandle depth_pipeline{};

  VulkanContext *context{};
  std::unique_ptr<EditorCamera> editor_camera;
  std::unique_ptr<ImGuiRenderer> imgui_renderer;
  std::unique_ptr<SceneRenderer> renderer;
  std::unique_ptr<GeometryPool> geo_pool;
  ViewportResources viewport_resources;

  VkExtent2D viewport_panel_extent{};
  VkExtent2D viewport_panel_offset{};
  VkExtent2D last_ui_size{};
  VkExtent2D last_ui_offset{};
  std::optional<VkExtent2D> pending_viewport_resize{std::nullopt};
  std::optional<VkExtent2D> candidate_viewport_resize{std::nullopt};

  double last_resize_change_time = 0.0;
  static constexpr double resize_debounce_delay = 0.1;

  ~Dockforge() override = default;

  auto init(const InitialisationContext &ctx) -> void override {
    renderer =
        std::make_unique<SceneRenderer>(ctx.context, ctx.swapchain_resources);
    constexpr auto vertex_count = 1'000'000;
    constexpr auto index_count = 10'000'000;
    constexpr auto material_count = 500;
    geo_pool = GeometryPool::create(
        ctx.context.allocator, vertex_count * sizeof(Vertex),
        vertex_count * sizeof(PositionOnlyVertex), index_count * sizeof(u32),
        material_count * sizeof(GPUMaterial));
    context = &ctx.context;
    imgui_renderer = std::make_unique<ImGuiRenderer>(
        get_window(), 16, *renderer,
        FontChoice{
            .font_path = VFSPath::create("fonts://some.ttf"),
            .size = 15.0f,
        });
    imgui_renderer->set_app_name("Dockforge");

    editor_scene = std::make_shared<Scene>();
    active_scene = editor_scene;
    active_scene->intialise_group<Components::Transform>();

    auto &&[w, h] = ctx.window_extent;

    editor_camera = std::make_unique<EditorCamera>(
        get_window(), glm::vec3{0.0f, 5.0f, -6.0f}, glm::vec3{0.0f, 0.0f, 0.0f},
        w, h);

    {
      const u32 white = 0xFFFFFFFF;
      auto white_texture = renderer->upload_texture(
          std::span(&white, 1), "white_fallback_texture", 1, 1,
          VK_FORMAT_B8G8R8A8_UNORM, false);
      info("Index: {}", white_texture.index());

      renderer->initialise_bindless(white_texture);
      viewport_resources = ViewportResources::create(*context, *renderer, w, h);
      renderer->update_output_texture(viewport_resources.forward_target);
      renderer->bindless.need_repopulate = true;
    }

    {
      auto &scene = *active_scene;
      const int grid_side = 20;
      const float spacing = glm::sqrt(2.F) + 0.5F;
      const float offset = (grid_side - 1) * spacing / 2.0f;

      for (int x = 0; x < grid_side; ++x) {
        for (int y = 0; y < grid_side; ++y) {
          for (int z = 0; z < grid_side; ++z) {
            auto name = std::format("Cube_{}_{}_{}", x, y, z);
            auto entity = scene.make(name);
            entity.get<Components::Transform>().position = {
                (static_cast<float>(x) * spacing) - offset,
                (static_cast<float>(y) * spacing) - offset,
                (static_cast<float>(z) * spacing) - offset,
            };
          }
        }
      }
    }

    auto &registry = *renderer->pipeline_registry;

    {
      auto result = registry.create_graphics({
          .shader_path = VFSPath::create("shaders://depth.slang"),
          .layout = renderer->pipeline_layout,
          .render_targets = {.depth_format = VK_FORMAT_D32_SFLOAT},
          .cull_mode = VK_CULL_MODE_BACK_BIT,
          .samples = VK_SAMPLE_COUNT_4_BIT,
          .depth = {.test = true,
                    .write = true,
                    .compare_op = VK_COMPARE_OP_GREATER_OR_EQUAL},
          .extra_dynamic_states = {VK_DYNAMIC_STATE_CULL_MODE,
                                   VK_DYNAMIC_STATE_FRONT_FACE},
      });
      if (!result) {
        error("depth prepass pipeline: {}", result.error());
        std::abort();
      }
      depth_pipeline = *result;
    }

    {
      auto result = registry.create_graphics({
          .shader_path = VFSPath::create("shaders://forward.slang"),
          .layout = renderer->pipeline_layout,
          .render_targets = {.color_formats = {VK_FORMAT_R16G16B16A16_SFLOAT},
                             .depth_format = VK_FORMAT_D32_SFLOAT},
          .cull_mode = VK_CULL_MODE_BACK_BIT,
          .samples = VK_SAMPLE_COUNT_4_BIT,
          .depth = {.test = true,
                    .write = false,
                    .compare_op = VK_COMPARE_OP_GREATER_OR_EQUAL},
          .blending = {BlendMode::opaque()},
          .extra_dynamic_states = {VK_DYNAMIC_STATE_CULL_MODE,
                                   VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
                                   VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
                                   VK_DYNAMIC_STATE_FRONT_FACE},
      });
      if (!result) {
        error("forward pipeline: {}", result.error());
        std::abort();
      }
      forward_pipeline = *result;
    }

    auto &&[vertex_offset, shadow_vertex_offset, index_offset] =
        geo_pool->allocate(cube_verts, cube_indices);
    mesh_handle = {
        .index_count = static_cast<u32>(std::ranges::size(cube_indices)),
        .first_index = static_cast<u32>(index_offset / sizeof(u32)),
        .vertex_offset = static_cast<i32>(vertex_offset / sizeof(Vertex)),
    };

    renderer->bindless.repopulate_if_needed(
        renderer->textures, renderer->samplers, renderer->comparison_samplers);
  }

  auto on_mouse_moved(const events::mouse_moved &e) -> void override {
    if (glfwGetMouseButton(get_window(), GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS)
      editor_camera->on_mouse_delta(e.dx, e.dy);
  }

  auto on_key_released(const events::key_released &e) -> void override {
    if (e.key == GLFW_KEY_ESCAPE)
      glfwSetWindowShouldClose(get_window(), GLFW_TRUE);

    if (e.key == GLFW_KEY_F2 && e.mods == GLFW_MOD_SHIFT)
      editor_camera->save_keyframe(2.0f);

    if (e.key == GLFW_KEY_F3 && e.mods == GLFW_MOD_SHIFT) {
      editor_camera->use_path = !editor_camera->use_path;
      if (editor_camera->use_path) {
        editor_camera->path_controller.current_index = 0;
        editor_camera->path_controller.segment_time = 0.0f;
        editor_camera->path_controller.is_playing = true;
      }
    }
  }

  [[nodiscard]] auto resolve_camera() const -> std::pair<glm::mat4, glm::mat4> {
    if (auto *cam = active_scene->primary_camera())
      return {cam->view(), cam->projection()};
    return {editor_camera->view(), editor_camera->projection()};
  }

  auto resize(u32 w, u32 h) -> void override {
    info("Dockforge resized to {}x{}", w, h);
    viewport_resources.resize(*context, *renderer, w, h);
    renderer->resize();
    editor_camera->set_aspect(w, h);
    for (auto &&[e, cam] : active_scene->view<Components::Camera>().each())
      cam.set_aspect(w, h);
  }

  auto build_ui() -> void {
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowViewport(vp->ID);

    ImGuiWindowFlags host_flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoDocking;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0.0f, 0.0f});
    ImGui::Begin("##DockHost", nullptr, host_flags);
    ImGui::PopStyleVar(3);

    ImGuiID dockspace_id = ImGui::GetID("MainDockspace");
    ImGui::DockSpace(dockspace_id, {0.0f, 0.0f}, ImGuiDockNodeFlags_None);
    ImGui::End();

    // ── Viewport panel ───────────────────────────────────────────────────
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0.0f, 0.0f});
    ImGui::Begin("Viewport");
    ImGui::PopStyleVar();

    const ImVec2 panel_size = ImGui::GetContentRegionAvail();
    const u32 panel_w = static_cast<u32>(panel_size.x);
    const u32 panel_h = static_cast<u32>(panel_size.y);

    if (panel_w > 0 && panel_h > 0) {
      if (panel_w != last_ui_size.width || panel_h != last_ui_size.height) {
        last_ui_size = {panel_w, panel_h};
        last_ui_offset = {
            static_cast<u32>(ImGui::GetCursorScreenPos().x),
            static_cast<u32>(ImGui::GetCursorScreenPos().y),
        };
        last_resize_change_time = glfwGetTime();
      }
    }

    ImGui::ImageButton("##viewport_main_image_button",
                       ImTextureRef{ImTextureID{
                           viewport_resources.display_target.index(),
                       }},
                       panel_size);

    ImGuizmo::SetDrawlist();
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetRect(ImGui::GetCursorScreenPos().x,
                      ImGui::GetCursorScreenPos().y, panel_size.x,
                      panel_size.y);

    ImGui::End();

    ImGui::ShowDemoWindow();

    if (ImGui::Begin("test")) {
      ImGui::Text("%d x %d", viewport_panel_extent.width,
                  viewport_panel_extent.height);
      ImGui::Text("%d x %d", viewport_panel_offset.width,
                  viewport_panel_offset.height);
      ImGui::End();
    }
  }

  auto destroy() -> void override {
    imgui_renderer.reset();

    viewport_resources.destroy(*context);
    renderer->destroy();

    editor_camera.reset();
    editor_scene.reset();
    runtime_scene.reset();
    active_scene.reset();
    geo_pool.reset();
    renderer.reset();
  }

  auto update(float ts) -> void override {
    if (!active_scene->primary_camera())
      editor_camera->update(ts);

    for (auto &&[e, xt] : active_scene->group<Components::Transform>().each()) {
      const float unique_seed =
          static_cast<float>(static_cast<u32>(e) % 100u) / 100.0f;
      const glm::vec3 local_axis =
          glm::normalize(glm::vec3(unique_seed, 1.0f, 0.5f));
      xt.rotation =
          glm::normalize(xt.rotation * glm::angleAxis(ts * 2.0f, local_axis));
    }
  }

  auto render(RenderContext &ctx) -> u64 override {
    double current_time = glfwGetTime();
    if ((last_ui_size.width != viewport_panel_extent.width ||
         last_ui_size.height != viewport_panel_extent.height)) {
      double time_since_last_move = current_time - last_resize_change_time;
      if (time_since_last_move > resize_debounce_delay) {
        viewport_resources.resize(*context, *renderer, last_ui_size.width,
                                  last_ui_size.height);
        renderer->resize();
        editor_camera->set_aspect(last_ui_size.width, last_ui_size.height);
        viewport_panel_extent = last_ui_size;
        viewport_panel_offset = last_ui_offset;
        info("Viewport resize {}x{}", viewport_panel_extent.width,
             viewport_panel_extent.height);
      }
    }

    constexpr u32 default_material = 0u;
    auto &scene = *active_scene;
    for (auto &&[e, xt] : scene
                              .view<const Components::Transform>(
                                  entt::exclude<Components::Camera>)
                              .each()) {
      renderer->submit(mesh_handle, xt, forward_pipeline.get(),
                       default_material);
    }

    auto [view, projection] = resolve_camera();
    renderer->prepare(ctx.frame_index, view, projection);

    if (renderer->bindless.repopulate_if_needed(
            renderer->textures, renderer->samplers,
            renderer->comparison_samplers)) {
      return ctx.next_frame_wait_value();
    }

    imgui_renderer->begin_frame(std::make_tuple(renderer->swapchain.extent(),
                                                renderer->swapchain.format()));
    build_ui();
    imgui_renderer->end_frame();

    const VkImageSubresourceRange color_range{.aspectMask =
                                                  VK_IMAGE_ASPECT_COLOR_BIT,
                                              .levelCount = 1u,
                                              .layerCount = 1u};
    const VkImageSubresourceRange depth_range{.aspectMask =
                                                  VK_IMAGE_ASPECT_DEPTH_BIT,
                                              .levelCount = 1u,
                                              .layerCount = 1u};
    const VkExtent2D vp_extent = viewport_resources.extent();
    const VkViewport viewport{0.0f,
                              static_cast<float>(vp_extent.height),
                              static_cast<float>(vp_extent.width),
                              -static_cast<float>(vp_extent.height),
                              0.0f,
                              1.0f};
    const VkRect2D scissor{{0, 0}, vp_extent};

    const auto &forward_texture =
        renderer->textures.get(renderer->forward_target_handle)->texture;
    const auto &display_texture =
        renderer->textures.get(viewport_resources.display_target)->texture;

    {
      const std::array<VkImageMemoryBarrier2, 2> initial_barriers{{
          {
              .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
              .dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                              VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
              .dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
              .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
              .newLayout = VK_IMAGE_LAYOUT_GENERAL,
              .image = viewport_resources.depth_msaa.image,
              .subresourceRange = depth_range,
          },
          {
              .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
              .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
              .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
              .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
              .newLayout = VK_IMAGE_LAYOUT_GENERAL,
              .image = viewport_resources.forward_target_msaa.image,
              .subresourceRange = color_range,
          },
      }};
      const VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                 .imageMemoryBarrierCount = 2u,
                                 .pImageMemoryBarriers =
                                     initial_barriers.data()};
      vkCmdPipelineBarrier2(ctx.main_cb, &dep);
    }

    // ── 2. Depth pre-pass ───────────────────────────────────────────────
    {
      const VkRenderingAttachmentInfo depth_attachment{
          .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
          .imageView = viewport_resources.depth_msaa.sampled_view,
          .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
          .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
          .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
          .clearValue = {.depthStencil = {0.0f, 0u}},
      };
      const VkRenderingInfo prepass_ri{
          .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
          .renderArea = scissor,
          .layerCount = 1u,
          .pDepthAttachment = &depth_attachment,
      };

      vkCmdBeginRendering(ctx.main_cb, &prepass_ri);
      vkCmdSetViewport(ctx.main_cb, 0u, 1u, &viewport);
      vkCmdSetScissor(ctx.main_cb, 0u, 1u, &scissor);
      vkCmdSetCullMode(ctx.main_cb, VK_CULL_MODE_BACK_BIT);
      vkCmdSetFrontFace(ctx.main_cb, VK_FRONT_FACE_CLOCKWISE);
      vkCmdBindIndexBuffer(ctx.main_cb, geo_pool->index_buffer->get_buffer(),
                           0u, VK_INDEX_TYPE_UINT32);
      renderer->render_pass(ctx.main_cb, ctx.frame_index, *geo_pool,
                            renderer->depth_prepass,
                            renderer->pipeline_registry->get(depth_pipeline));
      vkCmdEndRendering(ctx.main_cb);
    }

    // ── 3. Forward resolve target barrier ──────────────────────────────
    {
      const VkImageMemoryBarrier2 forward_target_barrier{
          .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
          .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
          .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
          .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
          .newLayout = VK_IMAGE_LAYOUT_GENERAL,
          .image = forward_texture.image,
          .subresourceRange = color_range,
      };
      const VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                 .imageMemoryBarrierCount = 1u,
                                 .pImageMemoryBarriers =
                                     &forward_target_barrier};
      vkCmdPipelineBarrier2(ctx.main_cb, &dep);
    }

    // ── 4. Forward MSAA pass ────────────────────────────────────────────
    {
      const VkRenderingAttachmentInfo forward_color{
          .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
          .imageView = viewport_resources.forward_target_msaa.sampled_view,
          .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
          .resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT,
          .resolveImageView = forward_texture.sampled_view,
          .resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL,
          .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
          .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
          .clearValue = {.color = {{0.0F, 0.0F, 0.0F, 0.0F}}},
      };
      const VkRenderingAttachmentInfo forward_depth{
          .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
          .imageView = viewport_resources.depth_msaa.sampled_view,
          .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
          .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
          .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      };
      const VkRenderingInfo forward_ri{
          .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
          .renderArea = scissor,
          .layerCount = 1u,
          .colorAttachmentCount = 1u,
          .pColorAttachments = &forward_color,
          .pDepthAttachment = &forward_depth,
      };

      vkCmdBeginRendering(ctx.main_cb, &forward_ri);
      vkCmdSetViewport(ctx.main_cb, 0u, 1u, &viewport);
      vkCmdSetScissor(ctx.main_cb, 0u, 1u, &scissor);
      vkCmdSetCullMode(ctx.main_cb, VK_CULL_MODE_BACK_BIT);
      vkCmdSetDepthCompareOp(ctx.main_cb, VK_COMPARE_OP_EQUAL);
      vkCmdSetDepthWriteEnable(ctx.main_cb, VK_FALSE);
      vkCmdSetFrontFace(ctx.main_cb, VK_FRONT_FACE_CLOCKWISE);
      vkCmdBindIndexBuffer(ctx.main_cb, geo_pool->index_buffer->get_buffer(),
                           0u, VK_INDEX_TYPE_UINT32);
      renderer->render_pass(ctx.main_cb, ctx.frame_index, *geo_pool,
                            renderer->forward_pass);
      vkCmdEndRendering(ctx.main_cb);
    }

    // ── 5. Composite pass → display_target ─────────────────────────────
    // forward_target (HDR) is sampled; display_target (LDR RGBA8) is written.
    // Two barriers in one call: forward_target read, display_target write.
    {
      const std::array<VkImageMemoryBarrier2, 2> composite_barriers{{
          {
              // forward_target: attachment write → shader sampled read
              .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
              .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
              .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
              .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
              .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
              .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
              .newLayout = VK_IMAGE_LAYOUT_GENERAL,
              .image = forward_texture.image,
              .subresourceRange = color_range,
          },
          {
              // display_target: UNDEFINED → color attachment write
              .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
              .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
              .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
              .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
              .newLayout = VK_IMAGE_LAYOUT_GENERAL,
              .image = display_texture.image,
              .subresourceRange = color_range,
          },
      }};
      const VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                 .imageMemoryBarrierCount = 2u,
                                 .pImageMemoryBarriers =
                                     composite_barriers.data()};
      vkCmdPipelineBarrier2(ctx.main_cb, &dep);

      const VkRenderingAttachmentInfo display_color{
          .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
          .imageView = display_texture.sampled_view,
          .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
          .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
          .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      };
      const VkRenderingInfo composite_ri{
          .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
          .renderArea = scissor,
          .layerCount = 1u,
          .colorAttachmentCount = 1u,
          .pColorAttachments = &display_color,
      };

      vkCmdBeginRendering(ctx.main_cb, &composite_ri);
      renderer->composite_pass(ctx.main_cb);
      vkCmdEndRendering(ctx.main_cb);
    }

    {
      const std::array<VkImageMemoryBarrier2, 2> swapchain_barriers{{
          {
              // display_target: attachment write → fragment shader sampled read
              .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
              .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
              .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
              .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
              .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
              .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
              .newLayout = VK_IMAGE_LAYOUT_GENERAL,
              .image = display_texture.image,
              .subresourceRange = color_range,
          },
          {
              // swapchain image: UNDEFINED → color attachment
              .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
              .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
              .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
              .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
              .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
              .image = ctx.swapchain_image.image,
              .subresourceRange = color_range,
          },
      }};
      const VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                 .imageMemoryBarrierCount = 2u,
                                 .pImageMemoryBarriers =
                                     swapchain_barriers.data()};
      vkCmdPipelineBarrier2(ctx.main_cb, &dep);

      const VkRenderingAttachmentInfo swapchain_color{
          .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
          .imageView = ctx.swapchain_image.view,
          .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
          .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
          .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      };
      const VkRenderingInfo swapchain_ri{
          .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
          .renderArea = {.extent = ctx.swapchain_image.extent},
          .layerCount = 1u,
          .colorAttachmentCount = 1u,
          .pColorAttachments = &swapchain_color,
      };

      vkCmdBeginRendering(ctx.main_cb, &swapchain_ri);
      imgui_renderer->render(ctx.main_cb);
      vkCmdEndRendering(ctx.main_cb);
    }

    // ── 7. Present barrier ──────────────────────────────────────────────
    {
      const VkImageMemoryBarrier2 present_barrier{
          .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
          .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
          .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
          .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
          .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
          .image = ctx.swapchain_image.image,
          .subresourceRange = color_range,
      };
      const VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                 .imageMemoryBarrierCount = 1u,
                                 .pImageMemoryBarriers = &present_barrier};
      vkCmdPipelineBarrier2(ctx.main_cb, &dep);
    }

    return ctx.next_frame_wait_value();
  }
};

auto main(int argc, char *argv[]) -> i32 {
  std::unique_ptr<App> app = std::make_unique<Dockforge>();
  return app->run(i32{argc}, argv);
}
#include "dockyard/compiler.hpp"
#include "dockyard/events.hpp"
#include "dockyard/vfs_path.hpp"
#include "glm/packing.hpp"
#include <algorithm>
#include <cassert>
#include <concepts>
#include <expected>
#include <numeric>
#include <ranges>
#include <utility>
#include <volk.h>

#include <entt/entt.hpp>

#include <dockyard/app.hpp>
#include <dockyard/bindless_descriptor.hpp>
#include <dockyard/buffer.hpp>
#include <dockyard/check_operation.hpp>
#include <dockyard/compiler.hpp>
#include <dockyard/context.hpp>
#include <dockyard/device_geometry.hpp>
#include <dockyard/scene_renderer.hpp>
#include <dockyard/texture.hpp>
#include <dockyard/types.hpp>
#include <dockyard/vfs.hpp>
#include <dockyard/vk_check.hpp>

#include <GLFW/glfw3.h>
#include <dockyard/scene.hpp>
#include <fstream>
#include <memory>

using namespace dy;

// ---------------------------------------------------------------------------
// Camera helpers
// ---------------------------------------------------------------------------

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

struct EditorCamera {
  Components::Camera camera{};
  FreeCameraController controller;

  EditorCamera(GLFWwindow *w, glm::vec3 position, glm::vec3 look_at, u32 width,
               u32 height)
      : controller(camera, w) {
    camera.position = position;
    camera.set_aspect(width, height);
    auto [y, p] = Components::Camera::facing_toward(position, look_at);
    camera.yaw = y;
    camera.pitch = p;
  }

  EditorCamera(const EditorCamera &) = delete;
  EditorCamera &operator=(const EditorCamera &) = delete;
  EditorCamera(EditorCamera &&) = delete;
  EditorCamera &operator=(EditorCamera &&) = delete;

  auto update(float ts) -> void { controller.update(ts); }
  auto on_mouse_delta(float dx, float dy) -> void {
    controller.on_mouse_delta(dx, dy);
  }
  auto set_aspect(u32 w, u32 h) -> void { camera.set_aspect(w, h); }
  [[nodiscard]] auto view() const -> glm::mat4 { return camera.view(); }
  [[nodiscard]] auto projection() const -> glm::mat4 {
    return camera.projection();
  }
};

auto create_depth_prepass_pipeline(VkDevice device, VkPipelineLayout layout,
                                   VkFormat depth_format) -> VkPipeline {
  auto compiled = shader::Compiler::the().compile("shaders://depth.slang");
  if (!compiled) {
    error("Could not runtime compile {} - {}", "shaders://depth.slang",
          compiled.error().message);
  }
  auto vs = TransientStage::create_from_entry_point(
      std::move(compiled->entry_points[0]));

  const VkPipelineShaderStageCreateInfo stages[1] = {vs.stage_ci};

  const VkPipelineVertexInputStateCreateInfo vertex_input{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  const VkPipelineInputAssemblyStateCreateInfo input_assembly{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
  };
  const VkPipelineDepthStencilStateCreateInfo depth_stencil{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = VK_TRUE,
      .depthWriteEnable = VK_TRUE,
      .depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL,
  };
  // No color attachments.
  const VkPipelineColorBlendStateCreateInfo color_blending{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 0u,
  };
  const std::array<VkDynamicState, 4> dynamic_states{
      VK_DYNAMIC_STATE_VIEWPORT,
      VK_DYNAMIC_STATE_SCISSOR,
      VK_DYNAMIC_STATE_CULL_MODE,
      VK_DYNAMIC_STATE_FRONT_FACE,
  };
  const VkPipelineDynamicStateCreateInfo dynamic_info{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
      .dynamicStateCount = static_cast<u32>(dynamic_states.size()),
      .pDynamicStates = dynamic_states.data(),
  };
  const VkPipelineViewportStateCreateInfo viewport_state{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1u,
      .scissorCount = 1u,
  };
  const VkPipelineMultisampleStateCreateInfo multisampling{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_4_BIT,
  };
  const VkPipelineRasterizationStateCreateInfo rasterizer{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = VK_CULL_MODE_BACK_BIT,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
      .lineWidth = 1.0f,
  };
  const VkPipelineRenderingCreateInfo rendering_info{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
      .colorAttachmentCount = 0u,
      .depthAttachmentFormat = depth_format,
  };

  const VkGraphicsPipelineCreateInfo pipeline_ci{
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .pNext = &rendering_info,
      .stageCount = 1u,
      .pStages = stages,
      .pVertexInputState = &vertex_input,
      .pInputAssemblyState = &input_assembly,
      .pViewportState = &viewport_state,
      .pRasterizationState = &rasterizer,
      .pMultisampleState = &multisampling,
      .pDepthStencilState = &depth_stencil,
      .pColorBlendState = &color_blending,
      .pDynamicState = &dynamic_info,
      .layout = layout,
      .renderPass = VK_NULL_HANDLE,
  };

  VkPipeline pipeline = VK_NULL_HANDLE;
  vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1u, &pipeline_ci, nullptr,
                            &pipeline);
  return pipeline;
}

auto create_forward_pipeline(VkDevice device, VkPipelineLayout layout,
                             VkFormat color_format, VkFormat depth_format)
    -> VkPipeline {
  auto compiled =
      shader::Compiler::the().compile("shaders://forward.slang").value();
  auto compiled_stages = TransientStage::create_all(std::move(compiled));

  std::array<VkPipelineShaderStageCreateInfo, 2> stages = {
      compiled_stages.at(0).stage_ci, compiled_stages.at(1).stage_ci};

  VkPipelineVertexInputStateCreateInfo vertex_input{};
  vertex_input.sType =
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  VkPipelineInputAssemblyStateCreateInfo input_assembly{};
  input_assembly.sType =
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineRasterizationStateCreateInfo rasterizer{};
  rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
  rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
  rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rasterizer.lineWidth = 1.0f;
  VkPipelineDepthStencilStateCreateInfo depth_stencil{};
  depth_stencil.sType =
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depth_stencil.depthTestEnable = VK_TRUE;
  depth_stencil.depthWriteEnable = VK_FALSE;
  depth_stencil.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
  VkPipelineColorBlendAttachmentState blend_attachment{};
  blend_attachment.colorWriteMask = 0xFu;
  blend_attachment.blendEnable = VK_FALSE;
  VkPipelineColorBlendStateCreateInfo color_blending{};
  color_blending.sType =
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  color_blending.attachmentCount = 1u;
  color_blending.pAttachments = &blend_attachment;
  const std::array<VkDynamicState, 6> dynamic_states{
      VK_DYNAMIC_STATE_VIEWPORT,           VK_DYNAMIC_STATE_SCISSOR,
      VK_DYNAMIC_STATE_CULL_MODE,          VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
      VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_FRONT_FACE,
  };
  VkPipelineDynamicStateCreateInfo dynamic_info{};
  dynamic_info.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamic_info.dynamicStateCount = static_cast<u32>(dynamic_states.size());
  dynamic_info.pDynamicStates = dynamic_states.data();
  VkPipelineViewportStateCreateInfo viewport_state{};
  viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport_state.viewportCount = 1u;
  viewport_state.scissorCount = 1u;
  VkPipelineMultisampleStateCreateInfo multisampling{};
  multisampling.sType =
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisampling.rasterizationSamples = VK_SAMPLE_COUNT_4_BIT;
  VkPipelineRenderingCreateInfo rendering_info{};
  rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
  rendering_info.colorAttachmentCount = 1u;
  rendering_info.pColorAttachmentFormats = &color_format;
  rendering_info.depthAttachmentFormat = depth_format;

  const VkGraphicsPipelineCreateInfo pipeline_ci{
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .pNext = &rendering_info,
      .stageCount = std::size(stages),
      .pStages = stages.data(),
      .pVertexInputState = &vertex_input,
      .pInputAssemblyState = &input_assembly,
      .pViewportState = &viewport_state,
      .pRasterizationState = &rasterizer,
      .pMultisampleState = &multisampling,
      .pDepthStencilState = &depth_stencil,
      .pColorBlendState = &color_blending,
      .pDynamicState = &dynamic_info,
      .layout = layout,
      .renderPass = VK_NULL_HANDLE,
  };

  VkPipeline pipeline = VK_NULL_HANDLE;
  vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1u, &pipeline_ci, nullptr,
                            &pipeline);
  return pipeline;
}

constexpr auto pack_normal(glm::vec3 n) {
  return glm::packSnorm4x8(glm::vec4(n, 0.0f));
}
constexpr auto pack_uv(glm::vec2 uv) { return glm::packHalf2x16(uv); }

static const std::vector<Vertex> cube_verts = {
    // Front Face (Z+)
    {{-0.5, -0.5, 0.5},
     pack_uv({0, 0}),
     pack_normal({0, 0, 1}),
     pack_normal({1, 0, 0}),
     pack_normal({0, 1, 0})},
    {{0.5, -0.5, 0.5},
     pack_uv({1, 0}),
     pack_normal({0, 0, 1}),
     pack_normal({1, 0, 0}),
     pack_normal({0, 1, 0})},
    {{0.5, 0.5, 0.5},
     pack_uv({1, 1}),
     pack_normal({0, 0, 1}),
     pack_normal({1, 0, 0}),
     pack_normal({0, 1, 0})},
    {{-0.5, 0.5, 0.5},
     pack_uv({0, 1}),
     pack_normal({0, 0, 1}),
     pack_normal({1, 0, 0}),
     pack_normal({0, 1, 0})},
    // Back Face (Z-)
    {{0.5, -0.5, -0.5},
     pack_uv({0, 0}),
     pack_normal({0, 0, -1}),
     pack_normal({-1, 0, 0}),
     pack_normal({0, 1, 0})},
    {{-0.5, -0.5, -0.5},
     pack_uv({1, 0}),
     pack_normal({0, 0, -1}),
     pack_normal({-1, 0, 0}),
     pack_normal({0, 1, 0})},
    {{-0.5, 0.5, -0.5},
     pack_uv({1, 1}),
     pack_normal({0, 0, -1}),
     pack_normal({-1, 0, 0}),
     pack_normal({0, 1, 0})},
    {{0.5, 0.5, -0.5},
     pack_uv({0, 1}),
     pack_normal({0, 0, -1}),
     pack_normal({-1, 0, 0}),
     pack_normal({0, 1, 0})},
    // Left Face (X-)
    {{-0.5, -0.5, -0.5},
     pack_uv({0, 0}),
     pack_normal({-1, 0, 0}),
     pack_normal({0, 0, 1}),
     pack_normal({0, 1, 0})},
    {{-0.5, -0.5, 0.5},
     pack_uv({1, 0}),
     pack_normal({-1, 0, 0}),
     pack_normal({0, 0, 1}),
     pack_normal({0, 1, 0})},
    {{-0.5, 0.5, 0.5},
     pack_uv({1, 1}),
     pack_normal({-1, 0, 0}),
     pack_normal({0, 0, 1}),
     pack_normal({0, 1, 0})},
    {{-0.5, 0.5, -0.5},
     pack_uv({0, 1}),
     pack_normal({-1, 0, 0}),
     pack_normal({0, 0, 1}),
     pack_normal({0, 1, 0})},
    // Right Face (X+)
    {{0.5, -0.5, 0.5},
     pack_uv({0, 0}),
     pack_normal({1, 0, 0}),
     pack_normal({0, 0, -1}),
     pack_normal({0, 1, 0})},
    {{0.5, -0.5, -0.5},
     pack_uv({1, 0}),
     pack_normal({1, 0, 0}),
     pack_normal({0, 0, -1}),
     pack_normal({0, 1, 0})},
    {{0.5, 0.5, -0.5},
     pack_uv({1, 1}),
     pack_normal({1, 0, 0}),
     pack_normal({0, 0, -1}),
     pack_normal({0, 1, 0})},
    {{0.5, 0.5, 0.5},
     pack_uv({0, 1}),
     pack_normal({1, 0, 0}),
     pack_normal({0, 0, -1}),
     pack_normal({0, 1, 0})},
    // Top Face (Y+)
    {{-0.5, 0.5, 0.5},
     pack_uv({0, 0}),
     pack_normal({0, 1, 0}),
     pack_normal({1, 0, 0}),
     pack_normal({0, 0, -1})},
    {{0.5, 0.5, 0.5},
     pack_uv({1, 0}),
     pack_normal({0, 1, 0}),
     pack_normal({1, 0, 0}),
     pack_normal({0, 0, -1})},
    {{0.5, 0.5, -0.5},
     pack_uv({1, 1}),
     pack_normal({0, 1, 0}),
     pack_normal({1, 0, 0}),
     pack_normal({0, 0, -1})},
    {{-0.5, 0.5, -0.5},
     pack_uv({0, 1}),
     pack_normal({0, 1, 0}),
     pack_normal({1, 0, 0}),
     pack_normal({0, 0, -1})},
    // Bottom Face (Y-)
    {{-0.5, -0.5, -0.5},
     pack_uv({0, 0}),
     pack_normal({0, -1, 0}),
     pack_normal({1, 0, 0}),
     pack_normal({0, 0, 1})},
    {{0.5, -0.5, -0.5},
     pack_uv({1, 0}),
     pack_normal({0, -1, 0}),
     pack_normal({1, 0, 0}),
     pack_normal({0, 0, 1})},
    {{0.5, -0.5, 0.5},
     pack_uv({1, 1}),
     pack_normal({0, -1, 0}),
     pack_normal({1, 0, 0}),
     pack_normal({0, 0, 1})},
    {{-0.5, -0.5, 0.5},
     pack_uv({0, 1}),
     pack_normal({0, -1, 0}),
     pack_normal({1, 0, 0}),
     pack_normal({0, 0, 1})},
};

static const std::vector<uint32_t> cube_indices = {
    0,  1,  2,  2,  3,  0,  // Front
    4,  5,  6,  6,  7,  4,  // Back
    8,  9,  10, 10, 11, 8,  // Left
    12, 13, 14, 14, 15, 12, // Right
    16, 17, 18, 18, 19, 16, // Top
    20, 21, 22, 22, 23, 20, // Bottom
};

constexpr auto operator==(const VkExtent2D &l, const VkExtent2D &r) -> bool {
  return l.width == r.width && l.height == r.height;
}

struct Dockforge : App {
  std::shared_ptr<Scene> editor_scene;
  std::shared_ptr<Scene> runtime_scene;
  std::shared_ptr<Scene> active_scene;

  GLFWwindow *main_window = nullptr;

  Mesh mesh_handle{};
  u32 forward_pipeline{};
  u32 depth_pipeline{};

  std::unique_ptr<EditorCamera> editor_camera;

  [[nodiscard]] auto resolve_camera() const -> std::pair<glm::mat4, glm::mat4> {
    if (auto *cam = active_scene->primary_camera())
      return {cam->view(), cam->projection()};
    return {editor_camera->view(), editor_camera->projection()};
  }

  VulkanContext *context{};
  std::unique_ptr<SceneRenderer> renderer;
  std::unique_ptr<GeometryPool> geo_pool;
  ViewportResources viewport_resources;

  ~Dockforge() override = default;

  auto init(const InitialisationContext &ctx) -> void override {
    renderer =
        std::make_unique<SceneRenderer>(ctx.context, ctx.swapchain_resources);
    constexpr auto vertex_count = 1'000'000;
    constexpr auto index_count = 10'000'000;
    geo_pool = GeometryPool::create(
        ctx.context.allocator, vertex_count * sizeof(Vertex),
        vertex_count * sizeof(PositionOnlyVertex), index_count * sizeof(u32));
    context = &ctx.context;

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

    auto &scene = *active_scene;
    const int grid_side = 20;
    const float spacing = glm::sqrt(2.F) + 0.5F;
    const float offset = (grid_side - 1) * spacing / 2.0f;

    for (int x = 0; x < grid_side; ++x) {
      for (int y = 0; y < grid_side; ++y) {
        for (int z = 0; z < grid_side; ++z) {

          auto name = std::format("Cube_{}_{}_{}", x, y, z);
          auto entity = scene.make(name);

          float posX = (static_cast<float>(x) * spacing) - offset;
          float posY = (static_cast<float>(y) * spacing) - offset;
          float posZ = (static_cast<float>(z) * spacing) - offset;

          entity.get<Components::Transform>().position = {posX, posY, posZ};
        }
      }
    }

    VkPipeline depth_pipe = create_depth_prepass_pipeline(
        ctx.context.device, renderer->pipeline_layout, VK_FORMAT_D32_SFLOAT);
    depth_pipeline = renderer->pipeline_registry->register_pipeline(depth_pipe);

    VkPipeline forward_pipe = create_forward_pipeline(
        ctx.context.device, renderer->pipeline_layout,
        VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D32_SFLOAT);
    forward_pipeline =
        renderer->pipeline_registry->register_pipeline(forward_pipe);

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
    if (e.key == GLFW_KEY_ESCAPE) {
      glfwSetWindowShouldClose(get_window(), GLFW_TRUE);
    }
  }

  auto resize(u32 w, u32 h) -> void override {
    info("Dockforge resized to {}x{}", w, h);
    viewport_resources.resize(*context, *renderer, w, h);
    //    const auto handle =
    //    renderer->textures.get(renderer->forward_target_handle);
    //    handle->sampled_view = viewport_resources.forward_target.view;
    //    handle->storage_view = viewport_resources.forward_target.view;

    renderer->resize();
    editor_camera->set_aspect(w, h);
    for (auto &&[e, cam] : active_scene->view<Components::Camera>().each())
      cam.set_aspect(w, h);
  }

  auto destroy() -> void override {
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

    auto &scene = *active_scene;
    auto view = scene.group<Components::Transform>();

    for (auto &&[e, xt] : view.each()) {
      float unique_seed = static_cast<float>((uint32_t)e % 100) / 100.0f;

      glm::vec3 local_axis = glm::normalize(glm::vec3(unique_seed, 1.0f, 0.5f));

      xt.rotation =
          glm::normalize(xt.rotation * glm::angleAxis(ts * 2.0f, local_axis));
    }
  }

  auto render(RenderContext &ctx) -> u64 override {
    constexpr u32 default_material = 0u;
    auto &scene = *active_scene;
    for (auto &&[e, xt] : scene
                              .view<const Components::Transform>(
                                  entt::exclude<Components::Camera>)
                              .each()) {
      renderer->submit(mesh_handle, xt, forward_pipeline, default_material);
    }

    auto [view, projection] = resolve_camera();
    renderer->prepare(ctx.frame_index, view, projection);

    if (renderer->bindless.repopulate_if_needed(
            renderer->textures, renderer->samplers,
            renderer->comparison_samplers)) {
      return ctx.next_frame_wait_value();
    }

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

    const auto &output_target =
        renderer->textures.get(renderer->forward_target_handle)->texture;

    {
      const std::array<VkImageMemoryBarrier2, 2> initial_barriers{
          {{
               .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
               .dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                               VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
               .dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
               .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
               .newLayout = VK_IMAGE_LAYOUT_GENERAL, // Unified Layout
               .image = viewport_resources.depth_msaa.image,
               .subresourceRange = depth_range,
           },
           {
               .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
               .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
               .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
               .newLayout = VK_IMAGE_LAYOUT_GENERAL, // Unified Layout
               .image = viewport_resources.forward_target_msaa.image,
               .subresourceRange = color_range,
           }}};
      const VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                 .imageMemoryBarrierCount = 2u,
                                 .pImageMemoryBarriers =
                                     initial_barriers.data()};
      vkCmdPipelineBarrier2(ctx.main_cb, &dep);
    }

    // --- 2. DEPTH PRE-PASS ---
    {
      const VkRenderingAttachmentInfo prepass_depth_attachment{
          .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
          .imageView = viewport_resources.depth_msaa.sampled_view,
          .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
          .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
          .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
          .clearValue = {.depthStencil = {0.0f, 0u}},
      };
      const VkRenderingInfo prepass_rendering_info{
          .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
          .renderArea = scissor,
          .layerCount = 1u,
          .pDepthAttachment = &prepass_depth_attachment,
      };

      vkCmdBeginRendering(ctx.main_cb, &prepass_rendering_info);
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

    // --- 3. FORWARD PASS BARRIER (Only for the Resolve Target) ---
    {
      const VkImageMemoryBarrier2 forward_target_barrier{
          .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
          .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
          .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
          .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
          .newLayout = VK_IMAGE_LAYOUT_GENERAL,
          .image = output_target.image,
          .subresourceRange = color_range,
      };
      const VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                 .imageMemoryBarrierCount = 1u,
                                 .pImageMemoryBarriers =
                                     &forward_target_barrier};
      vkCmdPipelineBarrier2(ctx.main_cb, &dep);
    }

    // --- 4. FORWARD MSAA PASS ---
    {
      const VkRenderingAttachmentInfo forward_color{
          .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
          .imageView = viewport_resources.forward_target_msaa.sampled_view,
          .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
          .resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT,
          .resolveImageView = output_target.sampled_view,
          .resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL,
          .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
          .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, // MSAA discarded via
                                                       // lazy backing
          .clearValue = {.color = {{0.39f, 0.58f, 0.93f, 1.0f}}},
      };
      const VkRenderingAttachmentInfo forward_depth{
          .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
          .imageView = viewport_resources.depth_msaa.sampled_view,
          .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
          .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
          .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, // Depth discarded via
                                                       // lazy backing
      };
      const VkRenderingInfo forward_rendering_info{
          .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
          .renderArea = scissor,
          .layerCount = 1u,
          .colorAttachmentCount = 1u,
          .pColorAttachments = &forward_color,
          .pDepthAttachment = &forward_depth,
      };

      vkCmdBeginRendering(ctx.main_cb, &forward_rendering_info);
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

    // --- 5. COMPOSITE PASS ---
    {
      const VkImageMemoryBarrier2 sc_barrier{
          .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
          .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
          .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
          .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
          .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,

          .image = ctx.swapchain_image.image,
          .subresourceRange = color_range,
      };
      const VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                 .imageMemoryBarrierCount = 1u,
                                 .pImageMemoryBarriers = &sc_barrier};
      vkCmdPipelineBarrier2(ctx.main_cb, &dep);

      const VkRenderingAttachmentInfo swapchain_color{
          .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
          .imageView = ctx.swapchain_image.view,
          .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
          .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
          .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      };
      const VkRenderingInfo swapchain_rendering_info{
          .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
          .renderArea = {.extent = ctx.swapchain_image.extent},
          .layerCount = 1u,
          .colorAttachmentCount = 1u,
          .pColorAttachments = &swapchain_color,
      };

      vkCmdBeginRendering(ctx.main_cb, &swapchain_rendering_info);
      renderer->composite_pass(ctx.main_cb);
      vkCmdEndRendering(ctx.main_cb);
    }

    // --- 6. PRESENT BARRIER ---
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
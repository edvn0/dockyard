#include "dockyard/events.hpp"
#include "glm/packing.hpp"
#include <cassert>
#include <concepts>
#include <expected>
#include <ranges>
#include <utility>
#include <volk.h>

#include <entt/entt.hpp>

#include <dockyard/app.hpp>
#include <dockyard/bindless_descriptor.hpp>
#include <dockyard/buffer.hpp>
#include <dockyard/check_operation.hpp>
#include <dockyard/context.hpp>
#include <dockyard/device_geometry.hpp>
#include <dockyard/types.hpp>
#include <dockyard/vfs.hpp>
#include <dockyard/vk_check.hpp>

#include <GLFW/glfw3.h>
#include <fstream>
#include <memory>
#include <vulkan/vulkan_core.h>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtx/quaternion.hpp>

using namespace dy;

// ---------------------------------------------------------------------------
// Components
// ---------------------------------------------------------------------------

namespace Components {

struct Camera {
  float fov_degrees = 70.0f; // DEGREES — never pass pre-converted radians here
  float near_plane = 0.7F;
  float far_plane = 1000.0f;
  float aspect = 16.0f / 9.0f;

  glm::vec3 position{0.0f};
  float yaw = 0.0f;   // radians
  float pitch = 0.0f; // radians

  [[nodiscard]] auto forward() const -> glm::vec3 {
    return glm::normalize(glm::vec3{
        glm::cos(pitch) * glm::sin(yaw),
        glm::sin(pitch),
        glm::cos(pitch) * glm::cos(yaw),
    });
  }

  [[nodiscard]] auto right() const -> glm::vec3 {
    return glm::normalize(glm::cross(glm::vec3{0.0f, 1.0f, 0.0f}, forward()));
  }

  [[nodiscard]] auto view() const -> glm::mat4 {
    return glm::lookAtLH(position, position + forward(),
                         glm::vec3{0.0f, 1.0f, 0.0f});
  }

  [[nodiscard]] auto projection() const -> glm::mat4 {
    return glm::perspectiveLH_ZO(glm::radians(fov_degrees), aspect, far_plane,
                                 near_plane);
  }

  auto set_aspect(u32 w, u32 h) -> void {
    aspect = static_cast<float>(w) / static_cast<float>(h);
  }

  static auto facing_toward(glm::vec3 from, glm::vec3 to)
      -> std::pair<float, float> {
    auto dir = glm::normalize(to - from);
    float p = glm::asin(glm::clamp(dir.y, -1.0f, 1.0f));
    float y = glm::atan(dir.x, dir.z);
    return {y, p};
  }
};

struct Tag {
  std::string tag;
  explicit Tag(const std::string_view n) : tag(n) {}
};

struct Transform {
  glm::vec3 position{};
  glm::quat rotation{};
  glm::vec3 scale{1};

  [[nodiscard]] auto matrix() const -> glm::mat4 {
    glm::mat4 res = glm::translate(glm::mat4(1.0f), position);
    res *= glm::mat4_cast(rotation);
    res = glm::scale(res, scale);
    return res;
  }
};

} // namespace Components

// ---------------------------------------------------------------------------
// Entity / Scene
// ---------------------------------------------------------------------------

class Entity {
  entt::registry &reg;
  entt::entity entity;

public:
  Entity(entt::registry &r, entt::entity_like auto identifier)
      : reg(r), entity(identifier) {}

  explicit Entity(entt::registry &r) : reg(r), entity(reg.create()) {}

  [[nodiscard]] auto handle() const -> entt::entity { return entity; }

  template <typename T, typename... Args>
  auto emplace(Args &&...args) -> decltype(auto) {
    return reg.emplace<T>(entity, std::forward<Args>(args)...);
  }
  template <typename T> auto get() -> decltype(auto) {
    return reg.get<T>(entity);
  }
  template <typename T> auto try_get() -> T * { return reg.try_get<T>(entity); }
};

class Scene {
  entt::registry scene;
  entt::entity primary_camera_entity{entt::null};

public:
  template <typename... Ts> auto view(auto &&...excludes) -> decltype(auto) {
    return scene.view<Ts...>(std::forward<decltype(excludes)>(excludes)...);
  }

  auto make(std::string_view name) -> Entity {
    auto e = Entity{scene};
    e.emplace<Components::Tag>(name);
    e.emplace<Components::Transform>();
    return e;
  }

  auto make_camera(std::string_view name, u32 width, u32 height,
                   glm::vec3 position = {0.0f, 0.0f, 0.0f},
                   glm::vec3 look_at = {0.0f, 0.0f, 1.0f}) -> Entity {
    auto e = make(name);
    auto &cam = e.emplace<Components::Camera>();
    cam.position = position;
    cam.set_aspect(width, height);
    auto [y, p] = Components::Camera::facing_toward(position, look_at);
    cam.yaw = y;
    cam.pitch = p;
    primary_camera_entity = e.handle();
    return e;
  }

  auto set_primary_camera(entt::entity e) -> void { primary_camera_entity = e; }
  auto clear_primary_camera() -> void { primary_camera_entity = entt::null; }

  [[nodiscard]] auto primary_camera() -> Components::Camera * {
    if (primary_camera_entity == entt::null)
      return nullptr;
    return scene.try_get<Components::Camera>(primary_camera_entity);
  }
};

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

// ---------------------------------------------------------------------------
// GPU types
// ---------------------------------------------------------------------------

struct Mesh {
  u32 index_count;
  u32 first_index;
  i32 vertex_offset;
  auto operator<=>(const Mesh &) const = default;
};

struct GpuPushConstants {
  const DeviceAddress vertex_buffer_ptr;               // 8 bytes
  const DeviceAddress position_only_vertex_buffer_ptr; // 8 bytes
  const DeviceAddress transform_buffer_ptr;            // 8 bytes
  const DeviceAddress remap_buffer_ptr;                // 8 bytes
  const DeviceAddress frame_ubo;                       // 8 bytes
};

struct CompositePushConstants {
  const TextureHandle forward_texture_index;
  const SamplerHandle sampler;
};

auto create_main_pipeline_layout(VkDevice device,
                                 VkDescriptorSetLayout bindless_layout)
    -> VkPipelineLayout {
  const VkPushConstantRange push_range{
      .stageFlags = VK_SHADER_STAGE_ALL,
      .offset = 0u,
      .size = sizeof(GpuPushConstants),
  };

  VkPipelineLayoutCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  ci.setLayoutCount = bindless_layout != VK_NULL_HANDLE ? 1u : 0u;
  ci.pSetLayouts = &bindless_layout, ci.pushConstantRangeCount = 1u;
  ci.pPushConstantRanges = &push_range;

  VkPipelineLayout layout = VK_NULL_HANDLE;
  vkCreatePipelineLayout(device, &ci, nullptr, &layout);
  return layout;
}

enum class RenderPassType { DepthPrepass, Forward, Shadow };

struct PendingDraw {
  Mesh mesh;
  u32 pipeline_id;
  u32 material_id;
  glm::mat4 transform;

  [[nodiscard]] constexpr auto get_key(RenderPassType pass) const -> u64 {
    const u32 mesh_id = mesh.first_index;
    if (pass == RenderPassType::DepthPrepass)
      return static_cast<u64>(mesh_id);
    return (static_cast<u64>(pipeline_id) << 48) |
           (static_cast<u64>(material_id) << 32) | mesh_id;
  }
};

struct RenderPass {
  RenderPassType type;
  VmaAllocator allocator;
  std::unique_ptr<Buffer> indirect_buffer{nullptr};
  std::unique_ptr<Buffer> index_remapping_buffer{nullptr};
  std::unique_ptr<Buffer> count_buffer{nullptr};

  struct Batch {
    u32 pipeline_id;
    u32 max_command_count;
    u32 first_command_index;
    u32 count_buffer_offset;
  };
  std::vector<Batch> batches{};

  void ensure_capacity(usize command_count, usize instance_count,
                       usize batch_count) {
    constexpr VkBufferUsageFlags indirect_flags =
        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    constexpr VkBufferUsageFlags storage_flags =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

    if (!indirect_buffer ||
        indirect_buffer->get_allocation_info().size <
            command_count * sizeof(VkDrawIndexedIndirectCommand)) {
      indirect_buffer = Buffer::create(
          allocator, command_count * sizeof(VkDrawIndexedIndirectCommand),
          indirect_flags);
    }
    if (!index_remapping_buffer ||
        index_remapping_buffer->get_allocation_info().size <
            instance_count * sizeof(u32)) {
      index_remapping_buffer = Buffer::create(
          allocator, instance_count * sizeof(u32), storage_flags);
    }
    if (!count_buffer ||
        count_buffer->get_allocation_info().size < batch_count * sizeof(u32)) {
      count_buffer =
          Buffer::create(allocator, batch_count * sizeof(u32), indirect_flags);
    }
  }

  void bake(const std::vector<PendingDraw> &scene_draws) {
    struct DrawRef {
      u32 global_idx;
      const PendingDraw *data;
    };

    std::vector<DrawRef> sorted_refs;
    sorted_refs.reserve(scene_draws.size());
    for (u32 i = 0u; i < scene_draws.size(); ++i)
      sorted_refs.push_back({i, &scene_draws[i]});

    std::ranges::sort(sorted_refs, [this](const auto &a, const auto &b) {
      return a.data->get_key(type) < b.data->get_key(type);
    });

    std::vector<VkDrawIndexedIndirectCommand> commands;
    std::vector<u32> remapped_indices;
    std::vector<u32> draw_counts;
    batches.clear();

    for (usize i = 0u; i < sorted_refs.size(); ++i) {
      const auto &[global_idx, data] = sorted_refs[i];
      const bool is_new_pipeline =
          i > 0u && data->pipeline_id != sorted_refs[i - 1].data->pipeline_id;
      const bool is_new_mesh =
          i > 0u &&
          data->mesh.first_index != sorted_refs[i - 1].data->mesh.first_index;

      if (is_new_pipeline || i == 0u) {
        const u32 count_idx = static_cast<u32>(draw_counts.size());
        draw_counts.push_back(0u);
        batches.push_back({
            data->pipeline_id,
            0u,
            static_cast<u32>(commands.size()),
            count_idx * static_cast<u32>(sizeof(u32)),
        });
      }

      if (is_new_mesh || is_new_pipeline || i == 0u) {
        commands.push_back({
            .indexCount = data->mesh.index_count,
            .instanceCount = 1u,
            .firstIndex = data->mesh.first_index,
            .vertexOffset = data->mesh.vertex_offset,
            .firstInstance = static_cast<u32>(remapped_indices.size()),
        });
        batches.back().max_command_count++;
        draw_counts.back()++;
      } else {
        commands.back().instanceCount++;
      }
      remapped_indices.push_back(global_idx);
    }

    ensure_capacity(commands.size(), remapped_indices.size(),
                    draw_counts.size());
    indirect_buffer->upload(commands);
    index_remapping_buffer->upload(remapped_indices);
    count_buffer->upload(draw_counts);
  }
};

struct InstanceData {
  glm::mat4 transform;
};

struct FrameUBO {
  glm::mat4 view;
  glm::mat4 projection;
  glm::mat4 view_projection;
  glm::mat4 inverse_projection;
  glm::mat4 inverse_view_projection;
};

struct PipelineRegistry {
  VkPipelineLayout &layout;
  VkDevice device;
  std::vector<VkPipeline> pipelines{};

  auto register_pipeline(VkPipeline pipeline) -> u32 {
    const u32 id = static_cast<u32>(pipelines.size());
    pipelines.push_back(pipeline);
    return id;
  }

  auto get(u32 id) const -> VkPipeline { return pipelines[id]; }

  auto cleanup() -> void {
    for (auto p : pipelines)
      vkDestroyPipeline(device, p, nullptr);
    pipelines.clear();
  }
};

struct TransientStage {
  std::vector<uint32_t> code;
  VkShaderModuleCreateInfo module_ci{};
  VkPipelineShaderStageCreateInfo stage_ci{};
  std::string entry_name;

  TransientStage() = default;

  TransientStage(TransientStage &&o) noexcept {
    code = std::move(o.code);
    entry_name = std::move(o.entry_name);
    module_ci = o.module_ci;
    module_ci.pCode = code.data();
    stage_ci = o.stage_ci;
    stage_ci.pName = entry_name.c_str();
    stage_ci.pNext = &module_ci;
  }

  TransientStage &operator=(TransientStage &&) = delete;
  TransientStage(const TransientStage &) = delete;

  static auto load(std::string_view virtual_path, VkShaderStageFlagBits stage,
                   std::string entry = "main")
      -> std::expected<TransientStage, std::string> {
    auto result = VFS::get().read_binary(virtual_path);
    if (!result) {
      return std::unexpected(result.error());
    }
    TransientStage ts{};
    ts.code = std::move(result.value());
    ts.entry_name = "main";
    ts.module_ci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .codeSize = ts.code.size() * sizeof(u32),
        .pCode = ts.code.data(),
    };
    ts.stage_ci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .pNext = &ts.module_ci,

        .flags = 0,
        .stage = stage,
        .module = VK_NULL_HANDLE,
        .pName = entry.c_str(),
        .pSpecializationInfo = nullptr,
    };
    return ts;
  }
};

auto create_depth_prepass_pipeline(VkDevice device, VkPipelineLayout layout,
                                   VkFormat depth_format) -> VkPipeline {
  auto maybe_vs = TransientStage::load("shaders://depth.vert.spv",
                                       VK_SHADER_STAGE_VERTEX_BIT);
  if (!maybe_vs)
    std::abort();

  auto vs = std::move(maybe_vs.value());
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
  auto maybe_vs = TransientStage::load("shaders://forward.vert.spv",
                                       VK_SHADER_STAGE_VERTEX_BIT);
  auto maybe_fs = TransientStage::load("shaders://forward.frag.spv",
                                       VK_SHADER_STAGE_FRAGMENT_BIT);
  if (!check_all(maybe_vs, maybe_fs))
    std::abort();

  auto vs = std::move(maybe_vs.value());
  auto fs = std::move(maybe_fs.value());
  const VkPipelineShaderStageCreateInfo stages[2] = {vs.stage_ci, fs.stage_ci};

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
      .stageCount = 2u,
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

auto create_composite_pipeline(VkDevice device,
                               VkDescriptorSetLayout bindless_layout)
    -> std::pair<VkPipelineLayout, VkPipeline> {
  const VkPushConstantRange push_range{
      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
      .offset = 0u,
      .size = sizeof(CompositePushConstants),
  };
  const VkPipelineLayoutCreateInfo layout_ci{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1u,
      .pSetLayouts = &bindless_layout,
      .pushConstantRangeCount = 1u,
      .pPushConstantRanges = &push_range,
  };
  VkPipelineLayout layout = VK_NULL_HANDLE;
  vkCreatePipelineLayout(device, &layout_ci, nullptr, &layout);

  auto maybe_vs = TransientStage::load("shaders://composite.vert.spv",
                                       VK_SHADER_STAGE_VERTEX_BIT);
  auto maybe_fs = TransientStage::load("shaders://composite.frag.spv",
                                       VK_SHADER_STAGE_FRAGMENT_BIT);

  if (!check_all(maybe_vs, maybe_fs)) {
    std::abort();
  }

  auto vs = std::move(maybe_vs.value());
  auto fs = std::move(maybe_fs.value());
  const VkPipelineShaderStageCreateInfo stages[2] = {vs.stage_ci, fs.stage_ci};

  const VkPipelineVertexInputStateCreateInfo vertex_input{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  const VkPipelineInputAssemblyStateCreateInfo input_assembly{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
  };
  const VkPipelineRasterizationStateCreateInfo rasterizer{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = VK_CULL_MODE_NONE,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
      .lineWidth = 1.0f,
  };
  const VkPipelineMultisampleStateCreateInfo multisampling{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
  };
  VkPipelineColorBlendAttachmentState blend_attachment{};
  blend_attachment.colorWriteMask = 0xFu;
  blend_attachment.blendEnable = VK_FALSE;
  const VkPipelineColorBlendStateCreateInfo color_blending{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1u,
      .pAttachments = &blend_attachment,
  };
  const std::array<VkDynamicState, 2> dynamic_states{
      VK_DYNAMIC_STATE_VIEWPORT,
      VK_DYNAMIC_STATE_SCISSOR,
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
  const VkFormat swapchain_format = VK_FORMAT_B8G8R8A8_SRGB;
  const VkPipelineRenderingCreateInfo rendering_info{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
      .colorAttachmentCount = 1u,
      .pColorAttachmentFormats = &swapchain_format,
  };

  const VkGraphicsPipelineCreateInfo pipeline_ci{
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .pNext = &rendering_info,
      .stageCount = 2u,
      .pStages = stages,
      .pVertexInputState = &vertex_input,
      .pInputAssemblyState = &input_assembly,
      .pViewportState = &viewport_state,
      .pRasterizationState = &rasterizer,
      .pMultisampleState = &multisampling,
      .pColorBlendState = &color_blending,
      .pDynamicState = &dynamic_info,
      .layout = layout,
      .renderPass = VK_NULL_HANDLE,
  };

  VkPipeline pipeline = VK_NULL_HANDLE;
  vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1u, &pipeline_ci, nullptr,
                            &pipeline);
  return {layout, pipeline};
}

// ---------------------------------------------------------------------------
// SceneRenderer
// ---------------------------------------------------------------------------

struct SceneRenderer {
  VulkanContext &ctx;
  SwapchainResources &swapchain;

  RenderPass depth_prepass;
  RenderPass forward_pass;

  TexturePool textures;
  SamplerPool samplers;
  ComparisonSamplerPool comparison_samplers;
  BindlessSet bindless;

  TextureHandle dummy_texture_handle{};
  SamplerHandle dummy_sampler_handle{};
  TextureHandle forward_target_handle{};
  VkSampler dummy_sampler_vk = VK_NULL_HANDLE;

  std::vector<InstanceData> global_transforms{};
  std::vector<PendingDraw> submission_queue{};
  std::unique_ptr<Buffer> global_instance_buffer{nullptr};
  std::vector<std::unique_ptr<Buffer>> frame_ubo_buffers{};

  VkPipelineLayout pipeline_layout{VK_NULL_HANDLE};
  std::unique_ptr<PipelineRegistry> pipeline_registry{nullptr};

  VkPipelineLayout composite_pipeline_layout{VK_NULL_HANDLE};
  VkPipeline composite_pipeline{VK_NULL_HANDLE};

  // -----------------------------------------------------------------------

  explicit SceneRenderer(VulkanContext &c, SwapchainResources &sc)
      : ctx(c), swapchain(sc),
        depth_prepass(RenderPassType::DepthPrepass, ctx.allocator),
        forward_pass(RenderPassType::Forward, ctx.allocator) {
    resize();
  }

  auto initialise_bindless(VkImageView forward_sampled,
                           VkImageView forward_storage) -> void {
    const VkSamplerCreateInfo sampler_ci{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    };
    vkCreateSampler(ctx.device, &sampler_ci, nullptr, &dummy_sampler_vk);

    dummy_texture_handle = textures.create(TextureEntry{
        .sampled_view = forward_sampled,
        .storage_view = forward_storage,
        .sampled_view_type = VK_IMAGE_VIEW_TYPE_2D,
    });
    dummy_sampler_handle =
        samplers.create(SamplerEntry{.sampler = dummy_sampler_vk});
    comparison_samplers.create(SamplerEntry{.sampler = dummy_sampler_vk});

    forward_target_handle = textures.create(TextureEntry{
        .sampled_view = forward_sampled,
        .storage_view = forward_storage,
        .sampled_view_type = VK_IMAGE_VIEW_TYPE_2D,
    });

    const BindlessCaps caps = query_bindless_caps(ctx.physical_device);
    bindless.init(ctx.device, caps,
                  /*initial_textures            =*/1024u,
                  /*initial_samplers            =*/64u,
                  /*initial_comparison_samplers =*/8u,
                  /*initial_storage_images      =*/512u,
                  /*initial_accel_structs       =*/0u);

    pipeline_layout = create_main_pipeline_layout(ctx.device, bindless.layout);
    pipeline_registry =
        std::make_unique<PipelineRegistry>(pipeline_layout, ctx.device);

    auto [comp_layout, comp_pipeline] =
        create_composite_pipeline(ctx.device, bindless.layout);
    composite_pipeline_layout = comp_layout;
    composite_pipeline = comp_pipeline;

    bindless.repopulate_if_needed(textures, samplers, comparison_samplers);
  }

  auto on_forward_target_changed(VkImageView new_sampled,
                                 VkImageView new_storage) -> void {
    if (auto *e = textures.get(dummy_texture_handle)) {
      e->sampled_view = new_sampled;
      e->storage_view = new_storage;
    }
    if (auto *e = textures.get(forward_target_handle)) {
      e->sampled_view = new_sampled;
      e->storage_view = new_storage;
    }
    bindless.need_repopulate = true;
  }

  auto resize() -> void {
    frame_ubo_buffers.clear();
    for (u32 i = 0u; i < frames_in_flight; ++i) {
      frame_ubo_buffers.emplace_back(Buffer::create(
          ctx.allocator, sizeof(FrameUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT));
    }
  }

  auto destroy() -> void {
    if (composite_pipeline != VK_NULL_HANDLE)
      vkDestroyPipeline(ctx.device, composite_pipeline, nullptr);
    if (composite_pipeline_layout != VK_NULL_HANDLE)
      vkDestroyPipelineLayout(ctx.device, composite_pipeline_layout, nullptr);
    if (dummy_sampler_vk != VK_NULL_HANDLE)
      vkDestroySampler(ctx.device, dummy_sampler_vk, nullptr);

    bindless.destroy();
    textures.clear();
    samplers.clear();
    comparison_samplers.clear();

    if (pipeline_registry)
      pipeline_registry->cleanup();
    if (pipeline_layout != VK_NULL_HANDLE)
      vkDestroyPipelineLayout(ctx.device, pipeline_layout, nullptr);
  }

  void submit(Mesh mesh, const Components::Transform &t, u32 p_id, u32 m_id) {
    submission_queue.emplace_back(mesh, p_id, m_id, t.matrix());
  }

  void ensure_global_capacity(usize instance_count) {
    if (const auto size = instance_count * sizeof(InstanceData);
        !global_instance_buffer ||
        global_instance_buffer->get_allocation_info().size < size) {
      global_instance_buffer = Buffer::create(
          ctx.allocator, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    }
  }

  void prepare(u64 frame_index, const glm::mat4 &view,
               const glm::mat4 &projection) {
    if (submission_queue.empty())
      return;

    ensure_global_capacity(submission_queue.size());
    global_transforms.clear();
    for (const auto &draw : submission_queue)
      global_transforms.emplace_back(draw.transform);
    global_instance_buffer->upload(global_transforms);

    const FrameUBO ubo{
        .view = view,
        .projection = projection,
        .view_projection = projection * view,
        .inverse_projection = glm::inverse(projection),
        .inverse_view_projection = glm::inverse(projection * view),
    };
    frame_ubo_buffers.at(frame_index)->upload(std::span(&ubo, 1));

    depth_prepass.bake(submission_queue);
    forward_pass.bake(submission_queue);
    submission_queue.clear();
  }

  void render_pass(VkCommandBuffer cmd, u64 frame_index, GeometryPool &pool,
                   RenderPass &pass,
                   VkPipeline override_pipeline = VK_NULL_HANDLE) {
    if (pass.batches.empty())
      return;

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipeline_layout, 0u, 1u, &bindless.set, 0u,
                            nullptr);

    const GpuPushConstants push_constants{
        .vertex_buffer_ptr =
            DeviceAddress{pool.vertex_buffer->get_device_address()},
        .position_only_vertex_buffer_ptr =
            DeviceAddress{
                pool.position_only_vertex_buffer->get_device_address()},
        .transform_buffer_ptr =
            DeviceAddress{global_instance_buffer->get_device_address()},
        .remap_buffer_ptr =
            DeviceAddress{pass.index_remapping_buffer->get_device_address()},
        .frame_ubo =
            DeviceAddress{
                frame_ubo_buffers.at(frame_index)->get_device_address()},
    };

    for (const auto &batch : pass.batches) {
      const VkPipeline pipe = override_pipeline != VK_NULL_HANDLE
                                  ? override_pipeline
                                  : pipeline_registry->get(batch.pipeline_id);
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
      vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_ALL, 0u,
                         sizeof(push_constants), &push_constants);
      vkCmdDrawIndexedIndirectCount(
          cmd, pass.indirect_buffer->get_buffer(),
          batch.first_command_index * sizeof(VkDrawIndexedIndirectCommand),
          pass.count_buffer->get_buffer(), batch.count_buffer_offset,
          batch.max_command_count, sizeof(VkDrawIndexedIndirectCommand));
    }
  }

  void composite_pass(VkCommandBuffer cmd) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, composite_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            composite_pipeline_layout, 0u, 1u, &bindless.set,
                            0u, nullptr);
    const CompositePushConstants push{
        .forward_texture_index = forward_target_handle,
        .sampler = dummy_sampler_handle,
    };
    vkCmdPushConstants(cmd, composite_pipeline_layout,
                       VK_SHADER_STAGE_FRAGMENT_BIT, 0u, sizeof(push), &push);
    vkCmdDraw(cmd, 3u, 1u, 0u, 0u);
  }
};

// ---------------------------------------------------------------------------
// Geometry data
// ---------------------------------------------------------------------------

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

    auto &&[w, h] = ctx.window_extent;

    editor_camera = std::make_unique<EditorCamera>(
        get_window(), glm::vec3{0.0f, 5.0f, -6.0f}, glm::vec3{0.0f, 0.0f, 0.0f},
        w, h);

    viewport_resources = ViewportResources::create(*context, w, h);
    renderer->initialise_bindless(
        viewport_resources.forward_target.view,
        viewport_resources.forward_target.view // storage = same for now
    );

    auto &scene = *active_scene;
    auto entity = scene.make("Something");
    entity.get<Components::Transform>().position = {0.0f, 0.0f, 0.0f};

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
    viewport_resources.resize(*context, w, h);
    renderer->resize();
    renderer->on_forward_target_changed(viewport_resources.forward_target.view,
                                        viewport_resources.forward_target.view);
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

    constexpr u32 default_material = 0u;
    auto &scene = *active_scene;

    for (auto &&[e, xt] : scene
                              .view<const Components::Transform>(
                                  entt::exclude<Components::Camera>)
                              .each()) {
      renderer->submit(mesh_handle, xt, forward_pipeline, default_material);
    }
  }

  auto render(RenderContext &ctx) -> u64 override {
    auto [view, projection] = resolve_camera();
    renderer->prepare(ctx.frame_index, view, projection);

    renderer->bindless.repopulate_if_needed(
        renderer->textures, renderer->samplers, renderer->comparison_samplers);

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

    // --- 1. PRE-PASS BARRIERS ---
    // Transition depth and MSAA targets to GENERAL
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
          .imageView = viewport_resources.depth_msaa.view,
          .imageLayout = VK_IMAGE_LAYOUT_GENERAL, // Must match the barrier!
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
          .image = viewport_resources.forward_target.image,
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
          .imageView = viewport_resources.forward_target_msaa.view,
          .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
          .resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT,
          .resolveImageView = viewport_resources.forward_target.view,
          .resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL,
          .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
          .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, // MSAA discarded via
                                                       // lazy backing
          .clearValue = {.color = {{0.39f, 0.58f, 0.93f, 1.0f}}},
      };
      const VkRenderingAttachmentInfo forward_depth{
          .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
          .imageView = viewport_resources.depth_msaa.view,
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
      // Transition swapchain for drawing
      const VkImageMemoryBarrier2 sc_barrier{
          .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
          .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
          .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
          .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
          .newLayout =
              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, // Swapchain usually
                                                        // stays optimal
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

    return ctx.wait_value + 1u;
  }
};

auto main(int argc, char *argv[]) -> i32 {
  std::unique_ptr<App> app = std::make_unique<Dockforge>();
  return app->run(i32{argc}, argv);
}
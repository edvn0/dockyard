#include "dockyard/vk_check.hpp"
#include <dockyard/pipeline_builder.hpp>

#include <dockyard/log.hpp>

#include <ranges>

namespace dy {

namespace {

template <std::ranges::random_access_range R,
          std::indirect_strict_weak_order<std::ranges::iterator_t<R>> Compare =
              std::ranges::less>
  requires std::ranges::sized_range<R>
auto sorted_copy(R range, Compare comp = {})
    -> std::vector<std::ranges::range_value_t<R>> {
  using value_type = std::ranges::range_value_t<R>;
  using diff_type = std::ranges::range_difference_t<R>;

  const auto n = static_cast<diff_type>(std::ranges::size(range));
  const auto first = std::ranges::begin(range);

  std::vector<diff_type> order(static_cast<usize>(n));
  std::ranges::iota(order.begin(), order.end(), diff_type{0});
  std::ranges::sort(order, [&](diff_type a, diff_type b) {
    return comp(first[a], first[b]);
  });

  std::vector<value_type> result;
  result.reserve(static_cast<usize>(n));
  for (auto idx : order) {
    if constexpr (std::ranges::view<R>)
      result.push_back(first[idx]);
    else
      result.push_back(std::move(first[idx]));
  }
  return result;
}

struct TransientStage {
  std::vector<u32> code{};
  VkShaderModuleCreateInfo module_ci{};
  VkPipelineShaderStageCreateInfo stage_ci{};
  std::string entry_name{};

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

  static constexpr auto to_vk_stage(shader::Stage stage)
      -> VkShaderStageFlagBits {
    switch (stage) {
    case shader::Stage::Vertex:
      return VK_SHADER_STAGE_VERTEX_BIT;
    case shader::Stage::Fragment:
      return VK_SHADER_STAGE_FRAGMENT_BIT;
    case shader::Stage::Mesh:
      return VK_SHADER_STAGE_MESH_BIT_EXT;
    case shader::Stage::Task:
      return VK_SHADER_STAGE_TASK_BIT_EXT;
    case shader::Stage::Compute:
      return VK_SHADER_STAGE_COMPUTE_BIT;
    }
  }

  static auto from_entry_point(shader::CompiledEntryPoint ep)
      -> TransientStage {
    TransientStage ts{};
    ts.code = std::move(ep.spirv);
    ts.entry_name = std::move(ep.entry_point.name);
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
        .stage = to_vk_stage(ep.entry_point.stage),
        .module = VK_NULL_HANDLE,
        .pName = ts.entry_name.c_str(),
        .pSpecializationInfo = nullptr,
    };
    return ts;
  }

  static auto compile_all(shader::CompiledShader shader)
      -> std::vector<TransientStage> {
    std::vector<TransientStage> stages;
    stages.reserve(shader.entry_points.size());
    for (auto &ep : shader.entry_points)
      stages.push_back(from_entry_point(std::move(ep)));

    return sorted_copy(std::move(stages),
                       [](const TransientStage &a, const TransientStage &b) {
                         return a.stage_ci.stage < b.stage_ci.stage;
                       });
  }
};

auto create_layout_from_reflection(
    VkDevice device, const shader::CompiledShader &compiled,
    VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE)
    -> std::expected<VkPipelineLayout, std::string> {
  u32 push_range_count = 0;
  VkPushConstantRange push_range{};

  if (compiled.push_constants.size > 0) {
    VkShaderStageFlags active_stages = 0;
    for (const auto &ep : compiled.entry_points) {
      active_stages |= TransientStage::to_vk_stage(ep.entry_point.stage);
    }

    push_range = {
        .stageFlags = active_stages,
        .offset = compiled.push_constants.offset,
        .size = compiled.push_constants.size,
    };
    push_range_count = 1;
  }

  const VkPipelineLayoutCreateInfo layout_ci{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = descriptor_set_layout != VK_NULL_HANDLE ? 1U : 0U,
      .pSetLayouts = descriptor_set_layout != VK_NULL_HANDLE
                         ? &descriptor_set_layout
                         : nullptr,
      .pushConstantRangeCount = push_range_count,
      .pPushConstantRanges = push_range_count > 0 ? &push_range : nullptr,
  };

  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
  if (vkCreatePipelineLayout(device, &layout_ci, nullptr, &pipeline_layout) !=
      VK_SUCCESS) {
    return std::unexpected(
        "vkCreatePipelineLayout failed during push-constant layout creation");
  }

  return pipeline_layout;
}

auto build_graphics_pipeline(VkDevice device,
                             const GraphicsPipelineDescription &desc,
                             VkPipelineLayout active_layout)
    -> std::expected<VkPipeline, std::string> {

  auto maybe_compiled = shader::Compiler::the().compile(desc.shader_path);
  if (!maybe_compiled)
    return std::unexpected(maybe_compiled.error().message);

  auto stages = TransientStage::compile_all(std::move(*maybe_compiled));

  std::vector<VkPipelineShaderStageCreateInfo> stage_cis;
  stage_cis.reserve(stages.size());
  for (const auto &s : stages)
    stage_cis.push_back(s.stage_ci);

  const auto n_color = desc.render_targets.color_formats.size();
  std::vector<VkPipelineColorBlendAttachmentState> blend_states;
  blend_states.reserve(n_color);
  for (usize i = 0u; i < n_color; ++i) {
    const BlendMode &mode =
        i < desc.blending.size() ? desc.blending[i] : BlendMode::opaque();
    blend_states.push_back(mode.to_vk());
  }

  VkPipelineVertexInputStateCreateInfo vertex_input{};
  vertex_input.sType =
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

  VkPipelineRasterizationLineStateCreateInfoKHR line_smoothness{};
  line_smoothness.sType =
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_LINE_STATE_CREATE_INFO;
  line_smoothness.stippledLineEnable =
      static_cast<VkBool32>(desc.topology == VK_PRIMITIVE_TOPOLOGY_LINE_LIST ||
                            desc.topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP);
  line_smoothness.lineRasterizationMode =
      VK_LINE_RASTERIZATION_MODE_RECTANGULAR_SMOOTH;
  line_smoothness.stippledLineEnable = VK_FALSE;
  line_smoothness.lineStipplePattern = 0xCC;
  line_smoothness.lineStippleFactor = 1;

  VkPipelineInputAssemblyStateCreateInfo input_assembly{};
  input_assembly.sType =
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  input_assembly.topology = desc.topology;
  VkPipelineViewportStateCreateInfo viewport_state{};
  viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport_state.viewportCount = 1u;
  viewport_state.scissorCount = 1u;
  VkPipelineRasterizationStateCreateInfo rasterizer{};
  rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rasterizer.pNext = desc.topology == VK_PRIMITIVE_TOPOLOGY_LINE_LIST ||
                             desc.topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP
                         ? &line_smoothness
                         : nullptr;
  rasterizer.polygonMode = desc.polygon_mode;
  rasterizer.cullMode = desc.cull_mode;
  rasterizer.frontFace = desc.front_face;
  rasterizer.lineWidth = desc.line_width;
  VkPipelineMultisampleStateCreateInfo multisampling{};
  multisampling.sType =
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisampling.rasterizationSamples = desc.samples;
  VkPipelineDepthStencilStateCreateInfo depth_stencil{};
  depth_stencil.sType =
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depth_stencil.depthTestEnable = desc.depth.test ? VK_TRUE : VK_FALSE;
  depth_stencil.depthWriteEnable = desc.depth.write ? VK_TRUE : VK_FALSE;
  depth_stencil.depthCompareOp = desc.depth.compare_op;
  VkPipelineColorBlendStateCreateInfo color_blending{};
  color_blending.sType =
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  color_blending.attachmentCount = static_cast<u32>(blend_states.size());
  color_blending.pAttachments = blend_states.data();
  std::vector<VkDynamicState> dynamic_states{
      VK_DYNAMIC_STATE_VIEWPORT,
      VK_DYNAMIC_STATE_SCISSOR,
  };
  dynamic_states.insert(dynamic_states.end(), desc.extra_dynamic_states.begin(),
                        desc.extra_dynamic_states.end());
  VkPipelineDynamicStateCreateInfo dynamic_info{};
  dynamic_info.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamic_info.dynamicStateCount = static_cast<u32>(dynamic_states.size());
  dynamic_info.pDynamicStates = dynamic_states.data();
  VkPipelineRenderingCreateInfo rendering_info{};
  rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
  rendering_info.colorAttachmentCount = static_cast<u32>(n_color);
  rendering_info.pColorAttachmentFormats =
      desc.render_targets.color_formats.data();
  rendering_info.depthAttachmentFormat = desc.render_targets.depth_format;
  rendering_info.stencilAttachmentFormat = desc.render_targets.stencil_format;
  VkGraphicsPipelineCreateInfo pipeline_ci{};
  pipeline_ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipeline_ci.pNext = &rendering_info;
  pipeline_ci.stageCount = static_cast<u32>(stage_cis.size());
  pipeline_ci.pStages = stage_cis.data();
  pipeline_ci.pVertexInputState = &vertex_input;
  pipeline_ci.pInputAssemblyState = &input_assembly;
  pipeline_ci.pViewportState = &viewport_state;
  pipeline_ci.pRasterizationState = &rasterizer;
  pipeline_ci.pMultisampleState = &multisampling;
  pipeline_ci.pDepthStencilState = &depth_stencil;
  pipeline_ci.pColorBlendState = &color_blending;
  pipeline_ci.pDynamicState = &dynamic_info;
  pipeline_ci.layout = active_layout;
  pipeline_ci.renderPass = VK_NULL_HANDLE;

  VkPipeline pipeline = VK_NULL_HANDLE;
  const auto vr = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1U,
                                            &pipeline_ci, nullptr, &pipeline);
  if (vr != VK_SUCCESS)
    return std::unexpected(std::format("vkCreateGraphicsPipelines failed ({})",
                                       static_cast<i32>(vr)));

  VkDebugUtilsObjectNameInfoEXT name_info{};
  name_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
  name_info.objectType = VK_OBJECT_TYPE_PIPELINE;
  name_info.objectHandle = reinterpret_cast<u64>(pipeline);
  std::string resolved_name = std::format(
      "{} ({} pipeline)", desc.shader_path.view(),
      desc.layout != VK_NULL_HANDLE ? "with user-provided layout"
                                    : "with auto-reflection layout");
  name_info.pObjectName = resolved_name.c_str();
  vk::check(vkSetDebugUtilsObjectNameEXT(device, &name_info));

  return pipeline;
}

auto build_compute_pipeline(VkDevice device,
                            const ComputePipelineDescription &desc,
                            VkPipelineLayout active_layout)
    -> std::expected<VkPipeline, std::string> {

  auto maybe_compiled = shader::Compiler::the().compile(desc.shader_path);
  if (!maybe_compiled)
    return std::unexpected(maybe_compiled.error().message);

  auto stages = TransientStage::compile_all(std::move(*maybe_compiled));

  if (stages.size() != 1u ||
      stages[0].stage_ci.stage != VK_SHADER_STAGE_COMPUTE_BIT)
    return std::unexpected(
        std::format("{}: expected exactly one compute entry point, got {}",
                    desc.shader_path.view(), stages.size()));

  const VkComputePipelineCreateInfo ci{
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = stages[0].stage_ci,
      .layout = active_layout,
  };

  VkPipeline pipeline = VK_NULL_HANDLE;
  const VkResult vr = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1u, &ci,
                                               nullptr, &pipeline);
  if (vr != VK_SUCCESS)
    return std::unexpected(std::format("vkCreateComputePipelines failed ({})",
                                       static_cast<i32>(vr)));

  VkDebugUtilsObjectNameInfoEXT name_info{};
  name_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
  name_info.objectType = VK_OBJECT_TYPE_PIPELINE;
  name_info.objectHandle = reinterpret_cast<u64>(pipeline);
  std::string resolved_name = std::format(
      "{} ({} pipeline)", desc.shader_path.view(),
      desc.layout != VK_NULL_HANDLE ? "with user-provided layout"
                                    : "with auto-reflection layout");
  name_info.pObjectName = resolved_name.c_str();
  vk::check(vkSetDebugUtilsObjectNameEXT(device, &name_info));

  return pipeline;
}

} // namespace

auto BlendMode::to_vk() const -> VkPipelineColorBlendAttachmentState {
  return {
      .blendEnable = enabled ? VK_TRUE : VK_FALSE,
      .srcColorBlendFactor = src_color,
      .dstColorBlendFactor = dst_color,
      .colorBlendOp = color_op,
      .srcAlphaBlendFactor = src_alpha,
      .dstAlphaBlendFactor = dst_alpha,
      .alphaBlendOp = alpha_op,
      .colorWriteMask = write_mask,
  };
}

auto PipelineRegistry::create_graphics(GraphicsPipelineDescription desc)
    -> std::expected<PipelineHandle, std::string> {

  auto maybe_compiled = shader::Compiler::the().compile(desc.shader_path);
  if (!maybe_compiled)
    return std::unexpected(maybe_compiled.error().message);

  VkPipelineLayout active_layout = desc.layout;
  bool owns_layout = false;

  if (active_layout == VK_NULL_HANDLE) {
    auto layout_res = create_layout_from_reflection(device, *maybe_compiled,
                                                    desc.descriptor_set_layout);
    if (!layout_res)
      return std::unexpected(layout_res.error());
    active_layout = *layout_res;
    owns_layout = true;
  }

  auto pipeline = build_graphics_pipeline(device, desc, active_layout);
  if (!pipeline) {
    if (owns_layout)
      vkDestroyPipelineLayout(device, active_layout, nullptr);
    return std::unexpected(std::move(pipeline.error()));
  }

  const u32 id = static_cast<u32>(entries.size());
  entries.push_back({.pipeline = *pipeline,
                     .layout = active_layout,
                     .owns_layout = owns_layout,
                     .desc = std::move(desc)});
  return id;
}

auto PipelineRegistry::create_compute(ComputePipelineDescription desc)
    -> std::expected<PipelineHandle, std::string> {

  auto maybe_compiled = shader::Compiler::the().compile(desc.shader_path);
  if (!maybe_compiled)
    return std::unexpected(maybe_compiled.error().message);

  VkPipelineLayout active_layout = desc.layout;
  bool owns_layout = false;

  if (active_layout == VK_NULL_HANDLE) {
    auto layout_res = create_layout_from_reflection(device, *maybe_compiled,
                                                    desc.descriptor_set_layout);
    if (!layout_res)
      return std::unexpected(layout_res.error());
    active_layout = *layout_res;
    owns_layout = true;
  }

  auto pipeline = build_compute_pipeline(device, desc, active_layout);
  if (!pipeline) {
    if (owns_layout)
      vkDestroyPipelineLayout(device, active_layout, nullptr);
    return std::unexpected(std::move(pipeline.error()));
  }

  const u32 id = static_cast<u32>(entries.size());
  entries.push_back({
      .pipeline = *pipeline,
      .layout = active_layout,
      .owns_layout = owns_layout,
      .desc = std::move(desc),
  });
  return id;
}

auto PipelineRegistry::reload(PipelineHandle id)
    -> std::expected<void, std::string> {
  if (!id.valid() || id.get() >= entries.size())
    return std::unexpected{"invalid handle"};

  auto &entry = entries[id.get()];

  auto path =
      std::visit([](const auto &desc) { return desc.shader_path; }, entry.desc);
  auto maybe_compiled = shader::Compiler::the().compile(path);
  if (!maybe_compiled)
    return std::unexpected(maybe_compiled.error().message);

  VkPipelineLayout original_provided_layout =
      std::visit([](const auto &desc) { return desc.layout; }, entry.desc);
  VkDescriptorSetLayout descriptor_set_layout = std::visit(
      [](const auto &desc) { return desc.descriptor_set_layout; }, entry.desc);

  VkPipelineLayout new_layout = original_provided_layout;
  bool new_owns_layout = false;

  if (new_layout == VK_NULL_HANDLE) {
    auto layout_res = create_layout_from_reflection(device, *maybe_compiled,
                                                    descriptor_set_layout);
    if (!layout_res)
      return std::unexpected(layout_res.error());
    new_layout = *layout_res;
    new_owns_layout = true;
  }

  auto new_pipeline = std::visit(
      [&](const auto &desc) -> std::expected<VkPipeline, std::string> {
        using T = std::decay_t<decltype(desc)>;
        if constexpr (std::is_same_v<T, GraphicsPipelineDescription>)
          return build_graphics_pipeline(device, desc, new_layout);
        else
          return build_compute_pipeline(device, desc, new_layout);
      },
      entry.desc);

  if (!new_pipeline) {
    if (new_owns_layout)
      vkDestroyPipelineLayout(device, new_layout, nullptr);
    return std::unexpected{new_pipeline.error()};
  }

  vkDeviceWaitIdle(device);

  if (entry.pipeline != VK_NULL_HANDLE) {
    vkDestroyPipeline(device, entry.pipeline, nullptr);
  }

  if (entry.owns_layout && entry.layout != VK_NULL_HANDLE) {
    vkDestroyPipelineLayout(device, entry.layout, nullptr);
  }

  entry.pipeline = *new_pipeline;
  entry.layout = new_layout;
  entry.owns_layout = new_owns_layout;

  return {};
}

auto PipelineRegistry::reload_all() -> void {
  for (u32 i = 0; i < static_cast<u32>(entries.size()); ++i) {
    if (auto r = reload(PipelineHandle{i}); !r)
      error("reload_all: entry {}: {}", i, r.error());
  }
}

auto PipelineRegistry::reload_by_shader(const VFSPath &path) -> void {
  for (auto i = 0U; i < entries.size(); i++) {
    auto &entry = entries.at(i);
    const auto matches = std::visit(
        [&](const auto &desc) {
          return desc.shader_path.view() == path.view();
        },
        entry.desc);

    if (!matches)
      continue;

    if (auto r = reload(PipelineHandle{static_cast<u32>(i)}); !r)
      error("shader reload failed ({}) pipeline {}: {}", path.view(), i,
            r.error());
    else
      info("reloaded pipeline {} ({})", i, path.view());
  }
}

auto PipelineRegistry::cleanup() -> void {
  for (auto &entry : entries)
    vkDestroyPipeline(device, entry.pipeline, nullptr);
  entries.clear();
}

} // namespace dy

#include <dockyard/pipeline_builder.hpp>

#include <dockyard/log.hpp>

#include <ranges>
#include <vulkan/vulkan_core.h>

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
        .codeSize = ts.code.size() * sizeof(u32),
        .pCode = ts.code.data(),
    };
    ts.stage_ci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .pNext = &ts.module_ci,
        .stage = to_vk_stage(ep.entry_point.stage),
        .module = VK_NULL_HANDLE,
        .pName = ts.entry_name.c_str(),
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

} // namespace

// ── BlendMode
// ─────────────────────────────────────────────────────────────────

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

auto build_graphics_pipeline(VkDevice device,
                             const GraphicsPipelineDescription &desc)
    -> std::expected<VkPipeline, std::string> {

  auto maybe_compiled = shader::Compiler::the().compile(desc.shader_path);
  if (!maybe_compiled)
    return std::unexpected(maybe_compiled.error().message);

  auto stages = TransientStage::compile_all(std::move(*maybe_compiled));

  std::vector<VkPipelineShaderStageCreateInfo> stage_cis;
  stage_cis.reserve(stages.size());
  for (const auto &s : stages)
    stage_cis.push_back(s.stage_ci);

  // Pad blend attachments with opaque() for any unspecified colour targets.
  const auto n_color = desc.render_targets.color_formats.size();
  std::vector<VkPipelineColorBlendAttachmentState> blend_states;
  blend_states.reserve(n_color);
  for (usize i = 0u; i < n_color; ++i) {
    const BlendMode &mode =
        i < desc.blending.size() ? desc.blending[i] : BlendMode::opaque();
    blend_states.push_back(mode.to_vk());
  }

  const VkPipelineVertexInputStateCreateInfo vertex_input{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
  };
  VkPipelineRasterizationLineStateCreateInfoKHR line_smoothness{};
  line_smoothness.sType =
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_LINE_STATE_CREATE_INFO;
  line_smoothness.stippledLineEnable =
      desc.topology == VK_PRIMITIVE_TOPOLOGY_LINE_LIST ||
      desc.topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
  line_smoothness.lineRasterizationMode =
      VK_LINE_RASTERIZATION_MODE_RECTANGULAR_SMOOTH;
  line_smoothness.stippledLineEnable = VK_FALSE;
  line_smoothness.lineStipplePattern = 0xCC;
  line_smoothness.lineStippleFactor = 1;

  const VkPipelineInputAssemblyStateCreateInfo input_assembly{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = desc.topology,
  };
  const VkPipelineViewportStateCreateInfo viewport_state{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1u,
      .scissorCount = 1u,
  };
  const VkPipelineRasterizationStateCreateInfo rasterizer{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .pNext = desc.topology == VK_PRIMITIVE_TOPOLOGY_LINE_LIST ||
                       desc.topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP
                   ? &line_smoothness
                   : nullptr,
      .polygonMode = desc.polygon_mode,
      .cullMode = desc.cull_mode,
      .frontFace = desc.front_face,
      .lineWidth = desc.line_width,
  };
  const VkPipelineMultisampleStateCreateInfo multisampling{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = desc.samples,
  };
  const VkPipelineDepthStencilStateCreateInfo depth_stencil{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = desc.depth.test ? VK_TRUE : VK_FALSE,
      .depthWriteEnable = desc.depth.write ? VK_TRUE : VK_FALSE,
      .depthCompareOp = desc.depth.compare_op,
  };
  const VkPipelineColorBlendStateCreateInfo color_blending{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = static_cast<u32>(blend_states.size()),
      .pAttachments = blend_states.data(),
  };
  std::vector<VkDynamicState> dynamic_states{
      VK_DYNAMIC_STATE_VIEWPORT,
      VK_DYNAMIC_STATE_SCISSOR,
  };
  dynamic_states.insert(dynamic_states.end(), desc.extra_dynamic_states.begin(),
                        desc.extra_dynamic_states.end());
  const VkPipelineDynamicStateCreateInfo dynamic_info{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
      .dynamicStateCount = static_cast<u32>(dynamic_states.size()),
      .pDynamicStates = dynamic_states.data(),
  };
  const VkPipelineRenderingCreateInfo rendering_info{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
      .colorAttachmentCount = static_cast<u32>(n_color),
      .pColorAttachmentFormats = desc.render_targets.color_formats.data(),
      .depthAttachmentFormat = desc.render_targets.depth_format,
      .stencilAttachmentFormat = desc.render_targets.stencil_format,
  };
  const VkGraphicsPipelineCreateInfo pipeline_ci{
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .pNext = &rendering_info,
      .stageCount = static_cast<u32>(stage_cis.size()),
      .pStages = stage_cis.data(),
      .pVertexInputState = &vertex_input,
      .pInputAssemblyState = &input_assembly,
      .pViewportState = &viewport_state,
      .pRasterizationState = &rasterizer,
      .pMultisampleState = &multisampling,
      .pDepthStencilState = &depth_stencil,
      .pColorBlendState = &color_blending,
      .pDynamicState = &dynamic_info,
      .layout = desc.layout,
      .renderPass = VK_NULL_HANDLE,
  };

  VkPipeline pipeline = VK_NULL_HANDLE;
  const VkResult vr = vkCreateGraphicsPipelines(
      device, VK_NULL_HANDLE, 1u, &pipeline_ci, nullptr, &pipeline);
  if (vr != VK_SUCCESS)
    return std::unexpected(std::format("vkCreateGraphicsPipelines failed ({})",
                                       static_cast<i32>(vr)));

  return pipeline;
}

// ── build_compute_pipeline
// ────────────────────────────────────────────────────

auto build_compute_pipeline(VkDevice device,
                            const ComputePipelineDescription &desc)
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
      .layout = desc.layout,
  };

  VkPipeline pipeline = VK_NULL_HANDLE;
  const VkResult vr = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1u, &ci,
                                               nullptr, &pipeline);
  if (vr != VK_SUCCESS)
    return std::unexpected(std::format("vkCreateComputePipelines failed ({})",
                                       static_cast<i32>(vr)));

  return pipeline;
}

// ── PipelineRegistry
// ──────────────────────────────────────────────────────────

auto PipelineRegistry::create_graphics(GraphicsPipelineDescription desc)
    -> std::expected<PipelineHandle, std::string> {
  auto pipeline = build_graphics_pipeline(device, desc);
  if (!pipeline)
    return std::unexpected(std::move(pipeline.error()));

  const u32 id = static_cast<u32>(entries.size());
  entries.push_back({*pipeline, std::move(desc)});
  return id;
}

auto PipelineRegistry::create_compute(ComputePipelineDescription desc)
    -> std::expected<PipelineHandle, std::string> {
  auto pipeline = build_compute_pipeline(device, desc);
  if (!pipeline)
    return std::unexpected(std::move(pipeline.error()));

  const u32 id = static_cast<u32>(entries.size());
  entries.push_back({*pipeline, std::move(desc)});
  return id;
}

auto PipelineRegistry::reload(PipelineHandle id)
    -> std::expected<void, std::string> {
  auto &entry = entries[id.get()];

  auto rebuild =
      [&](const auto &desc) -> std::expected<VkPipeline, std::string> {
    using T = std::decay_t<decltype(desc)>;
    if constexpr (std::is_same_v<T, GraphicsPipelineDescription>)
      return build_graphics_pipeline(device, desc);
    else
      return build_compute_pipeline(device, desc);
  };

  auto new_pipeline = std::visit(rebuild, entry.desc);
  if (!new_pipeline)
    return std::unexpected(std::move(new_pipeline.error()));

  // Swap only after successful compile — old pipeline survives a failed reload.
  vkDestroyPipeline(device, entry.pipeline, nullptr);
  entry.pipeline = *new_pipeline;
  return {};
}

auto PipelineRegistry::reload_all() -> void {
  for (u32 i = 0u; i < static_cast<u32>(entries.size()); ++i) {
    if (auto result = reload(i); !result)
      error("pipeline {}: hot-reload failed: {}", i, result.error());
  }
}

auto PipelineRegistry::cleanup() -> void {
  for (auto &entry : entries)
    vkDestroyPipeline(device, entry.pipeline, nullptr);
  entries.clear();
}

} // namespace dy
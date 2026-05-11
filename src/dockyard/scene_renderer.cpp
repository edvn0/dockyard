#include <dockyard/scene_renderer.hpp>

namespace dy {

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

  auto maybe_compiled =
      shader::Compiler::the().compile("shaders://composite.slang");
  if (!maybe_compiled) {
    error("{}", maybe_compiled.error().message);
    std::abort();
  }
  auto compiled = std::move(maybe_compiled.value());

  auto compiled_stages = TransientStage::create_all(std::move(compiled));

  const std::array<VkPipelineShaderStageCreateInfo, 2> stages = {
      compiled_stages.at(0).stage_ci,
      compiled_stages.at(1).stage_ci,
  };

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
      .stageCount = std::size(stages),
      .pStages = stages.data(),
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

} // namespace dy
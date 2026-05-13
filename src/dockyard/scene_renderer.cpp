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
                               VkDescriptorSetLayout bindless_layout,
                               VkPipelineLayout &out_layout,
                               VkPipeline &out_pipeline) -> void {
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
  vkCreatePipelineLayout(device, &layout_ci, nullptr, &out_layout);

  auto result = build_graphics_pipeline(
      device,
      GraphicsPipelineDescription{
          .shader_path = VFSPath::create("shaders://composite.slang"),
          .layout = out_layout,
          .render_targets = {.color_formats = {VK_FORMAT_R8G8B8A8_UNORM}},
          .cull_mode = VK_CULL_MODE_NONE,
          .blending = {BlendMode::opaque()},
      });
  if (!result) {
    error("composite pipeline: {}", result.error());
    std::abort();
  }
  out_pipeline = *result;
}

auto SceneRenderer::register_gltf(MeshAsset &&asset) -> MeshHandle {
  u32 id = static_cast<u32>(mesh_registry.size());
  mesh_registry.push_back(std::move(asset));
  return MeshHandle{id};
}
auto SceneRenderer::get_mesh(MeshHandle handle) -> MeshAsset * {
  if (!handle.valid() || handle.index() >= mesh_registry.size())
    return nullptr;
  return &mesh_registry[handle.index()];
}

SceneRenderer::SceneRenderer(VulkanContext &c, SwapchainResources &sc)
    : ctx(c), swapchain(sc),
      depth_prepass(RenderPassType::DepthPrepass, ctx.allocator),
      forward_pass(RenderPassType::Forward, ctx.allocator) {
  constexpr auto vertex_count = 1'000'000;
  constexpr auto index_count = 10'000'000;
  constexpr auto material_count = 500;
  geometry_pool = GeometryPool::create(
      ctx.allocator, vertex_count * sizeof(Vertex),
      vertex_count * sizeof(PositionOnlyVertex), index_count * sizeof(u32),
      material_count * sizeof(GPUMaterial));
  resize();
}
auto SceneRenderer::initialise_bindless(TextureHandle white) -> void {
  dummy_texture_handle = white;

  const VkSamplerCreateInfo sampler_ci{
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_LINEAR,
      .minFilter = VK_FILTER_LINEAR,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
      .mipLodBias = 0.0f,
      .anisotropyEnable = VK_TRUE,
      .maxAnisotropy = 16,
      .compareEnable = VK_FALSE,
      .minLod = 0.0f,
      .maxLod = 1.0f,
      .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
      .unnormalizedCoordinates = VK_FALSE,
  };
  vk::check(
      vkCreateSampler(ctx.device, &sampler_ci, nullptr, &dummy_sampler_vk));
  dummy_sampler_handle =
      samplers.create(SamplerEntry{.sampler = dummy_sampler_vk});

  const VkSamplerCreateInfo comparison_ci{
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_LINEAR,
      .minFilter = VK_FILTER_LINEAR,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .compareEnable = VK_TRUE,
      .compareOp = VK_COMPARE_OP_GREATER_OR_EQUAL,
      .minLod = 0.0f,
      .maxLod = 1.0f,
      .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,
  };
  vk::check(vkCreateSampler(ctx.device, &comparison_ci, nullptr,
                            &comparison_sampler_vk));
  comparison_samplers.create(SamplerEntry{.sampler = comparison_sampler_vk});

  const BindlessCaps caps = query_bindless_caps(ctx.physical_device);
  bindless.init(ctx.device, caps,
                /*initial_textures            =*/1024u,
                /*initial_samplers            =*/64u,
                /*initial_comparison_samplers =*/8u,
                /*initial_storage_images      =*/512u,
                /*initial_accel_structs       =*/0u);

  pipeline_layout = create_main_pipeline_layout(ctx.device, bindless.layout);
  pipeline_registry = std::make_unique<PipelineRegistry>(ctx.device);

  create_composite_pipeline(ctx.device, bindless.layout,
                            composite_pipeline_layout, composite_pipeline);
}
auto SceneRenderer::upload_texture(std::span<const u32> data,
                                   std::string_view name, u32 w, u32 h,
                                   VkFormat fmt, bool gen_mips, bool storage)
    -> TextureHandle {
  auto tex =
      Texture::from_bytes(ctx, name,
                          {
                              .bytes = data,
                              .width = w,
                              .height = h,
                              .format = fmt,
                              .generate_mips = gen_mips,
                              .storage_view = storage,
                              .upload_queue = ctx.graphics_queue(),
                              .upload_queue_family = ctx.graphics_queue_index,
                          });

  bindless.need_repopulate = true;
  return textures.create(TextureEntry{
      .texture = std::move(tex),
      .sampled_view_type = VK_IMAGE_VIEW_TYPE_2D,
  });
}
auto SceneRenderer::resize() -> void {
  frame_ubo_buffers.clear();
  for (u32 i = 0u; i < frames_in_flight; ++i) {
    frame_ubo_buffers.emplace_back(Buffer::create(
        ctx.allocator, sizeof(FrameUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT));
  }
  bindless.need_repopulate = true;
}
auto SceneRenderer::destroy() -> void {
  if (composite_pipeline != VK_NULL_HANDLE)
    vkDestroyPipeline(ctx.device, composite_pipeline, nullptr);
  if (composite_pipeline_layout != VK_NULL_HANDLE)
    vkDestroyPipelineLayout(ctx.device, composite_pipeline_layout, nullptr);

  bindless.destroy();

  std::ranges::for_each(textures.mutable_data(), [&c = ctx](auto &v) {
    auto &&[tex, view] = v.object;
    tex.destroy(c);
  });
  std::ranges::for_each(samplers.mutable_data(), [&c = ctx](auto &v) {
    auto &&[sampler] = v.object;
    DeletionQueue::get().push(
        [dev = c.device, s = sampler] { vkDestroySampler(dev, s, nullptr); });
  });
  std::ranges::for_each(
      comparison_samplers.mutable_data(), [&c = ctx](auto &v) {
        auto &&[sampler] = v.object;
        DeletionQueue::get().push([dev = c.device, s = sampler] {
          vkDestroySampler(dev, s, nullptr);
        });
      });

  if (pipeline_registry)
    pipeline_registry->cleanup();
  if (pipeline_layout != VK_NULL_HANDLE)
    vkDestroyPipelineLayout(ctx.device, pipeline_layout, nullptr);
}
void SceneRenderer::submit(MeshHandle handle, const Components::Transform &t,
                           u32 p_id, u32 m_id) {
  auto *asset = get_mesh(handle);
  if (!asset)
    return;

  for (const auto &node : asset->nodes) {
    glm::mat4 node_transform = t.matrix() * node.local_transform;

    for (const auto &prim : node.primitives) {
      submission_queue.emplace_back(
          prim.mesh, p_id, m_id != 0 ? m_id : prim.material_id, node_transform);
    }
  }
}
void SceneRenderer::submit(const Mesh &mesh, const Components::Transform &t,
                           u32 p_id, u32 m_id) {
  submission_queue.emplace_back(mesh, p_id, m_id, t.matrix());
}
void SceneRenderer::ensure_global_capacity(usize instance_count) {
  if (const auto size = instance_count * sizeof(InstanceData);
      !global_instance_buffer || global_instance_buffer->size() < size) {
    global_instance_buffer =
        Buffer::create(ctx.allocator, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  }
}
void SceneRenderer::prepare(u64 frame_index, const glm::mat4 &view,
                            const glm::mat4 &projection) {
  if (submission_queue.empty())
    return;

  ensure_global_capacity(submission_queue.size());
  global_instance_data.clear();
  for (const auto &draw : submission_queue)
    global_instance_data.emplace_back(draw.transform, draw.material_id);
  global_instance_buffer->upload(global_instance_data);

  const glm::mat4 inv_view = glm::inverse(view);
  const glm::mat4 inv_proj = glm::inverse(projection);
  const glm::mat4 view_proj = projection * view;
  const FrameUBO ubo{
      .view = view,
      .projection = projection,
      .view_projection = view_proj,
      .inverse_projection = inv_proj,
      .inverse_view = inv_view,
      .inverse_view_projection = glm::inverse(view_proj),
      .camera_position = inv_view[3],
  };
  frame_ubo_buffers.at(frame_index)->upload(std::span(&ubo, 1));

  depth_prepass.bake(submission_queue);
  forward_pass.bake(submission_queue);
  submission_queue.clear();
}
void SceneRenderer::render_pass(VkCommandBuffer cmd, u64 frame_index,
                                GeometryPool &pool, RenderPass &pass,
                                VkPipeline override_pipeline) {
  if (pass.batches.empty())
    return;

  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout,
                          0u, 1u, &bindless.set, 0u, nullptr);

  const GpuPushConstants push_constants{
      .vertex_buffer_ptr =
          DeviceAddress{
              pool.vertex_buffer->get_device_address(),
          },
      .position_only_vertex_buffer_ptr =
          DeviceAddress{
              pool.position_only_vertex_buffer->get_device_address(),
          },
      .transform_buffer_ptr =
          DeviceAddress{
              global_instance_buffer->get_device_address(),
          },
      .remap_buffer_ptr =
          DeviceAddress{
              pass.index_remapping_buffer->get_device_address(),
          },
      .frame_ubo =
          DeviceAddress{
              frame_ubo_buffers.at(frame_index)->get_device_address(),
          },
      .material_ptr =
          DeviceAddress{
              pool.material_buffer->get_device_address(),
          },
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
void SceneRenderer::composite_pass(VkCommandBuffer cmd) {
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, composite_pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          composite_pipeline_layout, 0u, 1u, &bindless.set, 0u,
                          nullptr);
  const CompositePushConstants push{
      .forward_texture_index = forward_target_handle,
      .sampler = dummy_sampler_handle,
  };
  vkCmdPushConstants(cmd, composite_pipeline_layout,
                     VK_SHADER_STAGE_FRAGMENT_BIT, 0u, sizeof(push), &push);
  vkCmdDraw(cmd, 3u, 1u, 0u, 0u);
}

void RenderPass::ensure_capacity(usize command_count, usize instance_count,
                                 usize batch_count) {
  constexpr VkBufferUsageFlags indirect_flags =
      VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  constexpr VkBufferUsageFlags storage_flags =
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

  if (!indirect_buffer ||
      indirect_buffer->size() <
          command_count * sizeof(VkDrawIndexedIndirectCommand)) {
    indirect_buffer = Buffer::create(
        allocator, command_count * sizeof(VkDrawIndexedIndirectCommand),
        indirect_flags);
  }
  if (!index_remapping_buffer ||
      index_remapping_buffer->size() < instance_count * sizeof(u32)) {
    index_remapping_buffer =
        Buffer::create(allocator, instance_count * sizeof(u32), storage_flags);
  }
  if (!count_buffer || count_buffer->size() < batch_count * sizeof(u32)) {
    count_buffer =
        Buffer::create(allocator, batch_count * sizeof(u32), indirect_flags);
  }
}
void RenderPass::bake(const std::vector<PendingDraw> &scene_draws) {
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

  ensure_capacity(commands.size(), remapped_indices.size(), draw_counts.size());
  indirect_buffer->upload(commands);
  index_remapping_buffer->upload(remapped_indices);
  count_buffer->upload(draw_counts);
}
[[nodiscard]] constexpr auto PendingDraw::get_key(RenderPassType pass) const
    -> u64 {
  const u32 mesh_id = mesh.first_index;
  if (pass == RenderPassType::DepthPrepass)
    return static_cast<u64>(mesh_id);
  return (static_cast<u64>(pipeline_id) << 48) |
         (static_cast<u64>(material_id) << 32) | mesh_id;
}
} // namespace dy
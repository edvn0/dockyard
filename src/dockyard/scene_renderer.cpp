#include <atomic>
#include <dockyard/scene_renderer.hpp>

#include <dockyard/device_geometry.hpp>
#include <limits>
#include <vulkan/vulkan_core.h>

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
auto SceneRenderer::register_external_view(VkImageView view,
                                           VkImageViewType type)
    -> TextureHandle {
  TextureEntry entry{
      .texture =
          {
              .sampled_view = view,
              .owned = false,
          },
      .sampled_view_type = type,
  };
  TextureHandle handle = textures.create(std::move(entry));

  bindless.queue_texture_write(handle.index(), view, VK_NULL_HANDLE, type);
  bindless.need_repopulate = true;
  return handle;
}

auto SceneRenderer::get_mesh(MeshHandle handle) -> MeshAsset * {
  if (!handle.valid() || handle.index() >= mesh_registry.size())
    return nullptr;
  return &mesh_registry[handle.index()];
}
auto SceneRenderer::get_mesh(MeshHandle handle) const -> const MeshAsset * {
  if (!handle.valid() || handle.index() >= mesh_registry.size())
    return nullptr;
  return &mesh_registry[handle.index()];
}

auto SceneRenderer::get_material_view(MeshHandle handle) const
    -> ConstMaterialView {
  const auto *mesh = get_mesh(handle);
  assert(mesh && "invalid mesh handle");
  return {
      .materials = geometry_pool->get_materials(mesh->material_base_slot,
                                                mesh->material_count),
      .base_slot = mesh->material_base_slot,
  };
}

auto SceneRenderer::get_material_view_mut(MeshHandle handle)
    -> MutableMaterialView {
  const auto *mesh = get_mesh(handle);
  assert(mesh && "invalid mesh handle");
  return {
      .materials = geometry_pool->get_materials_mut(mesh->material_base_slot,
                                                    mesh->material_count),
      .base_slot = mesh->material_base_slot,
  };
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

  const VkSamplerCreateInfo shadow_ci{
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_LINEAR,
      .minFilter = VK_FILTER_LINEAR,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
      .compareEnable = VK_TRUE,
      .compareOp = VK_COMPARE_OP_LESS_OR_EQUAL, // shadow maps are conventional
      .minLod = 0.0f,
      .maxLod = 1.0f,
      .borderColor =
          VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE, // 1.0 = fully lit outside
  };
  vk::check(vkCreateSampler(ctx.device, &shadow_ci, nullptr,
                            &shadow_comparison_sampler_vk));
  shadow_sampler_bindless_idx =
      comparison_samplers
          .create(SamplerEntry{.sampler = shadow_comparison_sampler_vk})
          .index();

  init_csm();

  {
    auto result = pipeline_registry->create_graphics({
        .shader_path = VFSPath::create("shaders://shadow.slang"),
        .layout = pipeline_layout,
        .render_targets = {.depth_format = VK_FORMAT_D32_SFLOAT},
        .cull_mode = VK_CULL_MODE_BACK_BIT,
        .samples = VK_SAMPLE_COUNT_1_BIT, // no MSAA on shadow maps
        .depth = {.test = true,
                  .write = true,
                  .compare_op = VK_COMPARE_OP_LESS_OR_EQUAL}, // conventional
        .extra_dynamic_states =
            {
                VK_DYNAMIC_STATE_CULL_MODE,
                VK_DYNAMIC_STATE_DEPTH_BIAS,
            },
    });
    if (!result) {
      error("shadow pipeline: {}", result.error());
      std::abort();
    }
    shadow_pipeline = *result;
  }
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
  csm.destroy(ctx.device, ctx.allocator);

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

namespace {
std::atomic_uint64_t current_frame_index{std::numeric_limits<u64>::max()};
}

void SceneRenderer::prepare(u64 frame_index, const glm::mat4 &view,
                            const glm::mat4 &projection) {
  current_frame_index = frame_index;
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
      // Just copy the struct directly — one memcpy worth of data
      .cascades = csm_frame_data.cascades,
      .shadow_array_index = csm_frame_data.shadow_array_index,
      .shadow_sampler_index = csm_frame_data.shadow_sampler_index,
  };

  frame_ubo_buffers.at(frame_index)->upload(std::span(&ubo, 1));
  depth_prepass.bake(submission_queue);
  forward_pass.bake(submission_queue);
  submission_queue.clear();
}
void SceneRenderer::render_pass(VkCommandBuffer cmd, RenderPass &pass,
                                VkPipeline override_pipeline) {
  auto &pool = *geometry_pool;
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
              frame_ubo_buffers.at(current_frame_index)->get_device_address(),
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

void SceneRenderer::render_shadow_cascade(VkCommandBuffer cmd,
                                          u32 cascade_idx) {
  auto &pass = depth_prepass;
  auto pipeline = pipeline_registry->get(shadow_pipeline);
  if (pass.batches.empty())
    return;

  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout,
                          0u, 1u, &bindless.set, 0u, nullptr);
  vkCmdBindIndexBuffer(cmd, geometry_pool->index_buffer->get_buffer(), 0u,
                       VK_INDEX_TYPE_UINT32);

  GpuPushConstants push{
      .vertex_buffer_ptr =
          DeviceAddress{geometry_pool->vertex_buffer->get_device_address()},
      .position_only_vertex_buffer_ptr =
          DeviceAddress{
              geometry_pool->position_only_vertex_buffer->get_device_address()},
      .transform_buffer_ptr =
          DeviceAddress{global_instance_buffer->get_device_address()},
      .remap_buffer_ptr =
          DeviceAddress{pass.index_remapping_buffer->get_device_address()},
      .frame_ubo =
          DeviceAddress{
              frame_ubo_buffers.at(current_frame_index)->get_device_address()},
      .material_ptr =
          DeviceAddress{geometry_pool->material_buffer->get_device_address()},
      .cascade_index = cascade_idx,
  };

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_ALL, 0u,
                     sizeof(push), &push);

  // Draw all batches (shadow doesn't care about per-batch pipeline variation)
  for (const auto &batch : pass.batches) {
    vkCmdDrawIndexedIndirectCount(
        cmd, pass.indirect_buffer->get_buffer(),
        batch.first_command_index * sizeof(VkDrawIndexedIndirectCommand),
        pass.count_buffer->get_buffer(), batch.count_buffer_offset,
        batch.max_command_count, sizeof(VkDrawIndexedIndirectCommand));
  }
}

auto compute_cascade_splits(float near, float far, float lambda = 0.85f) {
  std::array<float, shadow_map_cascade_count> splits{};
  float range = far - near;
  float ratio = far / near;

  for (u32 i = 0; i < shadow_map_cascade_count; ++i) {
    float p = static_cast<float>(i + 1) /
              static_cast<float>(shadow_map_cascade_count);
    float log_split = near * std::pow(ratio, p);
    float uni_split = near + range * p;
    splits[i] = lambda * log_split + (1.0f - lambda) * uni_split;
  }
  return splits;
}

std::array<glm::vec3, 8> frustum_corners_world(const glm::mat4 &inv_view_proj,
                                               float z_start, float z_end) {
  std::array<glm::vec3, 8> corners;

  // NDC corners in LH: -1 to 1 for X/Y
  // Z is whatever you pass in (for Rev-Z: Near=1, Far=0)
  glm::vec4 ndc[8] = {{-1, 1, z_start, 1}, {1, 1, z_start, 1},
                      {1, -1, z_start, 1}, {-1, -1, z_start, 1},
                      {-1, 1, z_end, 1},   {1, 1, z_end, 1},
                      {1, -1, z_end, 1},   {-1, -1, z_end, 1}};

  for (u32 i = 0; i < 8; ++i) {
    glm::vec4 world = inv_view_proj * ndc[i];
    // Critical: Check for w=0 to avoid NaN if splits are at infinity
    float w = std::abs(world.w) > 0.00001f ? world.w : 1.0f;
    corners[i] = glm::vec3(world) / w;
  }
  return corners;
}

CascadeData compute_cascade(const glm::mat4 &camera_view,
                            const glm::mat4 &camera_proj, float prev_split_ndc,
                            float curr_split_ndc, float curr_split_view,
                            const glm::vec3 &light_dir) {
  glm::mat4 inv_view_proj = glm::inverse(camera_proj * camera_view);
  auto corners =
      frustum_corners_world(inv_view_proj, prev_split_ndc, curr_split_ndc);

  // 1. Calculate center
  glm::vec3 center(0.0f);
  for (const auto &c : corners)
    center += c;
  center /= 8.0f;

  // 2. Calculate radius of the bounding sphere
  float radius = 0.0f;
  for (const auto &c : corners) {
    radius = std::max(radius, glm::distance(c, center));
  }
  // Round up the radius slightly to prevent edge flickering
  radius = std::ceil(radius * 16.0f) / 16.0f;

  // 3. Create stable light view
  glm::vec3 up =
      std::abs(light_dir.y) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
  // Pull back far enough to see the whole sphere
  float pull_back_distance = radius + 1000.0f;
  glm::mat4 light_view =
      glm::lookAtLH(center - (light_dir * pull_back_distance), center, up);

  // 4. Determine Ortho bounds based on Radius
  // Since we are looking at 'center', the center in light-space is (0,0,Z)
  // We want our ortho box to be [-radius, radius]
  float texels_per_unit =
      static_cast<float>(shadow_map_cascade_resolution) / (radius * 2.0f);

  // 5. Texel Snapping
  // We snap the light-space origin so that texels move in discrete increments
  glm::mat4 shadow_matrix = light_view;
  glm::vec4 shadow_origin = shadow_matrix * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
  shadow_origin *= texels_per_unit;

  glm::vec4 rounded_origin = glm::round(shadow_origin);
  glm::vec4 round_offset = rounded_origin - shadow_origin;
  round_offset /= texels_per_unit;
  round_offset.z = 0.0f;
  round_offset.w = 0.0f;

  // Apply snapping offset to the light view matrix
  light_view[3] += round_offset;

  // 6. Final Ortho Projection
  // Use radius for X and Y to keep the box size constant
  float l = -radius;
  float r = radius;
  float b = -radius;
  float t = radius;

  // Z range: should cover the entire sphere volume + pancake
  float n = -radius - 1000.0f;
  float f = radius + 1000.0f;

  const auto light_proj = glm::orthoLH_ZO(l, r, b, t, n, f);

  return {
      .view_proj = light_proj * light_view,
      .split_depth = curr_split_view,
  };
}

constexpr float split_to_ndc_z(const glm::mat4 &proj, float view_z) {
  // proj * (0,0,view_z,1), take z/w
  glm::vec4 clip = proj * glm::vec4(0.0f, 0.0f, view_z, 1.0f);
  return clip.z / clip.w;
}

void SceneRenderer::update_csm(const glm::mat4 &view, const glm::mat4 &proj,
                               float camera_near, float camera_far) {
  auto splits = compute_cascade_splits(camera_near, camera_far);

  // In Reversed-Z LH:
  // Near plane (camera_near) -> NDC 1.0
  // Far plane (camera_far)   -> NDC 0.0
  float prev_ndc = 1.0f;

  for (u32 i = 0u; i < shadow_map_cascade_count; ++i) {
    float view_z = splits[i]; // Positive Z for LH
    float curr_ndc = split_to_ndc_z(proj, view_z);

    // We pass (Near, Far) to compute_cascade
    // For cascade 0: prev_ndc is 1.0, curr_ndc might be 0.8
    csm_frame_data.cascades[i] =
        compute_cascade(view, proj, prev_ndc, curr_ndc, view_z, sun_direction);

    prev_ndc = curr_ndc;
  }

  csm_frame_data.shadow_array_index = csm.bindless_handle.index();
  csm_frame_data.shadow_sampler_index = shadow_sampler_bindless_idx;
}

auto CsmResources::destroy(VkDevice device, VmaAllocator allocator) -> void {
  vmaDestroyImage(allocator, image, allocation);
  for (auto &v : layer_views) {
    vkDestroyImageView(device, v, nullptr);
  }
  vkDestroyImageView(device, array_view, nullptr);
}

void SceneRenderer::init_csm() {
  const VkImageCreateInfo image_ci{
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_D32_SFLOAT,
      .extent =
          {
              shadow_map_cascade_resolution,
              shadow_map_cascade_resolution,
              1u,
          },
      .mipLevels = 1u,
      .arrayLayers = shadow_map_cascade_count,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
               VK_IMAGE_USAGE_SAMPLED_BIT,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
  };
  const VmaAllocationCreateInfo alloc_ci{
      .usage = VMA_MEMORY_USAGE_GPU_ONLY,
  };
  vmaCreateImage(ctx.allocator, &image_ci, &alloc_ci, &csm.image,
                 &csm.allocation, nullptr);

  const VkImageViewCreateInfo array_view_ci{
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = csm.image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
      .format = VK_FORMAT_D32_SFLOAT,
      .subresourceRange =
          {
              .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
              .levelCount = 1u,
              .layerCount = shadow_map_cascade_count,
          },
  };
  vkCreateImageView(ctx.device, &array_view_ci, nullptr, &csm.array_view);

  // Per-layer views for rendering into each cascade
  for (u32 i = 0u; i < shadow_map_cascade_count; ++i) {
    const VkImageViewCreateInfo layer_ci{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = csm.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_D32_SFLOAT,
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                .levelCount = 1u,
                .baseArrayLayer = i,
                .layerCount = 1u,
            },
    };
    vkCreateImageView(ctx.device, &layer_ci, nullptr, &csm.layer_views[i]);
  }

  csm.bindless_handle =
      register_external_view(csm.array_view, VK_IMAGE_VIEW_TYPE_2D_ARRAY);
  info("CSM handle: {}", csm.bindless_handle.index());
  bindless.need_repopulate = true;

  ctx.transition_to_general(csm.image);
}

} // namespace dy
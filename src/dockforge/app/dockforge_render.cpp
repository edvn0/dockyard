#include <dockforge/dockforge.hpp>

#include <dockforge/editor_camera.hpp>

#include <dockyard/animation.hpp>
#include <dockyard/components.hpp>
#include <dockyard/imgui_renderer.hpp>
#include <dockyard/scene_renderer.hpp>

using namespace dy;

namespace {
auto resize_viewport(Dockforge &app) -> void {
  double current_time = glfwGetTime();
  double time_since_last_move = current_time - app.last_resize_change_time;
  if (time_since_last_move > Dockforge::resize_debounce_delay) {
    app.viewport_resources.resize(*app.context, *app.renderer,
                                  app.last_ui_size.width,
                                  app.last_ui_size.height);
    app.renderer->resize();
    app.editor_camera->set_aspect(app.last_ui_size.width,
                                  app.last_ui_size.height);
    app.viewport_panel_extent = app.last_ui_size;
    app.viewport_panel_offset = app.last_ui_offset;
    trace("Viewport resize {}x{}", app.viewport_panel_extent.width,
          app.viewport_panel_extent.height);
  }
}

auto resolve_material_slot(Entity e) -> u32 {
  auto *ov = e.try_get<Components::MaterialOverride>();
  if (ov == nullptr ||
      ov->gpu_slot == Components::MaterialOverride::invalid_material)
    return Components::MaterialOverride::invalid_material;
  return ov->gpu_slot;
}
} // namespace

static void emit_barrier(VkCommandBuffer cmd,
                         const VkImageMemoryBarrier2 &image_barrier) {
  const VkDependencyInfo dependency_info{
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .pNext = nullptr,
      .dependencyFlags = 0,
      .memoryBarrierCount = 0,
      .pMemoryBarriers = nullptr,
      .bufferMemoryBarrierCount = 0,
      .pBufferMemoryBarriers = nullptr,
      .imageMemoryBarrierCount = 1,
      .pImageMemoryBarriers = &image_barrier,
  };

  vkCmdPipelineBarrier2(cmd, &dependency_info);
}

static void compute_world_matrices(entt::registry &registry) {
  ZoneScopedNC("compute_world_matrices", 0x90EE90);
  auto base_view =
      registry.view<Components::Transform, Components::LocalToWorld>();
  for (auto &&[entity, xt, ltw] : base_view.each()) {
    ltw.matrix = xt.matrix();
  }

  std::unordered_map<entt::entity, std::vector<entt::entity>> children;
  auto relation_view = registry.view<Components::ParentOf>();
  for (auto entity : relation_view) {
    auto &rel = relation_view.get<Components::ParentOf>(entity);
    if (registry.valid(rel.parent))
      children[rel.parent].push_back(entity);
  }

  // propagate from roots only — avoids re-multiplying already-resolved children
  std::function<void(entt::entity)> propagate = [&](entt::entity e) {
    auto *parent_ltw = registry.try_get<Components::LocalToWorld>(e);
    if (!parent_ltw)
      return;
    auto it = children.find(e);
    if (it == children.end())
      return;
    for (auto child : it->second) {
      auto *child_ltw = registry.try_get<Components::LocalToWorld>(child);
      if (child_ltw)
        child_ltw->matrix = parent_ltw->matrix * child_ltw->matrix;
      propagate(child);
    }
  };

  for (auto entity : base_view) {
    if (!registry.any_of<Components::ParentOf>(entity))
      propagate(entity);
  }
}

void Dockforge::flush_material_overrides() {
  ZoneScopedNC("Dockforge::flush_material_overrides", 0xFF8C00);
  auto view = active_scene->registry().view<Components::MaterialOverride>();
  for (auto &&[e, override_slot] : view.each()) {
    if (override_slot.gpu_slot ==
        Components::MaterialOverride::invalid_material) {
      if (auto slot = renderer->override_pool.alloc()) {
        override_slot.gpu_slot = *slot;
        override_slot.dirty = true;
      } else {
        warn("MaterialOverridePool full - override skipped this frame");
      }
    }
    if (override_slot.dirty &&
        override_slot.gpu_slot !=
            Components::MaterialOverride::invalid_material) {
      renderer->geometry_pool->get_materials_mut(override_slot.gpu_slot, 1)[0] =
          override_slot.material;
      renderer->geometry_pool->flush_material(override_slot.gpu_slot);
      override_slot.dirty = false;
    }
  }
}

void Dockforge::patch_material_override_slots(u32 delta) {
  ZoneScopedNC("Dockforge::patch_material_override_slots", 0xFF8C00);
  auto view = active_scene->registry().view<Components::MaterialOverride>();
  for (auto &&[e, override_slot] : view.each()) {
    if (override_slot.gpu_slot !=
        Components::MaterialOverride::invalid_material) {
      override_slot.gpu_slot += delta;
      override_slot.dirty = true;
    }
  }
}

auto Dockforge::render(RenderContext &ctx) -> u64 {
  ZoneScopedNC("Dockforge::render", 0xFF6347);
  if (editor_state.hierarchy_dirty) [[unlikely]] {
    compute_world_matrices(active_scene->registry());
    shadow_map_state.invalid = true;
    editor_state.hierarchy_dirty = false;
  }

  if (pending_pick) [[unlikely]] {
    try_pick_entity(*pending_pick);
    pending_pick.reset();
  }

  const bool size_changed =
      (last_ui_size.width != viewport_panel_extent.width ||
       last_ui_size.height != viewport_panel_extent.height);
  if (size_changed) [[unlikely]] {
    TracyMessage("Viewport resized", 16);
    resize_viewport(*this);
  }

  auto render_view =
      active_scene->view<Components::Transform, Components::LocalToWorld,
                         Components::Mesh>(entt::exclude<AnimationState>);

  for (auto &&[e, xt, ltw, m] : render_view.each()) {
    renderer->submit(m.handle, ltw.matrix, forward_pipeline.index(),
                     resolve_material_slot({*active_scene, e}));
  }

  for (auto &&[e, xt, ltw, m, anim] :
       active_scene
           ->view<Components::Transform, Components::LocalToWorld,
                  Components::Mesh, AnimationState>()
           .each()) {
    renderer->submit(
        m.handle, ltw.matrix, std::span<const glm::mat4>(anim.joint_palette),
        forward_pipeline.index(), resolve_material_slot({*active_scene, e}));
  }
  auto [view, projection] = resolve_camera();
  const auto camera_moved = view != shadow_map_state.last_view_matrix;
  if (camera_moved) {
    shadow_map_state.last_view_matrix = view;
    shadow_map_state.invalid = true;
  }
  if (shadow_map_state.invalid) {
    renderer->update_csm(view, projection, editor_camera->near_plane(),
                         shadow_map_state.far_plane);
  }

  std::array<GPUPointLight, FrameUBO::max_point_lights> gpu_lights;
  u32 light_count = 0;
  for (auto &&[e, ltw, light] : active_scene
                                    ->view<const Components::LocalToWorld,
                                           const Components::PointLight>()
                                    .each()) {
    if (light_count >= FrameUBO::max_point_lights)
      break;
    gpu_lights[light_count++] = {
        .position = glm::vec4(ltw.matrix[3]), // extract translation
        .radius = light.radius,
        .color = light.color,
        .intensity = light.intensity,
    };
  }

  TracyPlot("point_lights", static_cast<int64_t>(light_count));
  TracyPlot("mesh_entities", static_cast<int64_t>(render_view.size_hint()));

  const VkExtent2D vp_extent = viewport_resources.extent();
  auto prepare_result = renderer->prepare({
      .frame_index = ctx.frame_index,
      .view = view,
      .projection = projection,
      .camera_near_far =
          glm::vec2(editor_camera->near_plane(), editor_camera->far_plane()),
      .shadow_near_far =
          glm::vec2(shadow_map_state.near_plane, shadow_map_state.far_plane),
      .point_lights = std::span(gpu_lights.data(), light_count),
      .viewport_size = glm::uvec2(vp_extent.width, vp_extent.height),
  });
  if (prepare_result.failed()) {
    return ctx.next_frame_wait_value();
  }

#undef Status
  if (prepare_result.status ==
          SceneRenderer::PrepareResult::Status::SuccessMaterialPoolGrew &&
      prepare_result.material_pool_delta > 0) {
    patch_material_override_slots(prepare_result.material_pool_delta);
  }

  flush_material_overrides();

  if (renderer->bindless.repopulate_if_needed(
          renderer->textures, renderer->samplers, renderer->comparison_samplers,
          renderer->subimages)) {
    return ctx.next_frame_wait_value();
  }

  imgui_renderer->begin_frame(std::make_tuple(renderer->swapchain.extent(),
                                              renderer->swapchain.format()));
  build_ui();
  imgui_renderer->end_frame();

  const VkImageSubresourceRange color_range{
      .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
      .baseMipLevel = 0U,
      .levelCount = 1U,
      .baseArrayLayer = 0U,
      .layerCount = 1U,
  };
  const VkViewport viewport{
      .x = 0.0F,
      .y = static_cast<float>(vp_extent.height),
      .width = static_cast<float>(vp_extent.width),
      .height = -static_cast<float>(vp_extent.height),
      .minDepth = 0.0F,
      .maxDepth = 1.0F,
  };
  const VkRect2D scissor{
      .offset =
          {
              .x = 0,
              .y = 0,
          },
      .extent = vp_extent,
  };

  const auto &forward_texture =
      renderer->resolve(renderer->forward_target_handle);
  const auto &display_texture =
      renderer->resolve(viewport_resources.display_target);
  {
    renderer->skinning_pass(ctx.main_cb);
  }
  {
    renderer->depth_frustum_culling_pass(ctx.main_cb);
  }

  if (shadow_map_state.invalid) {
    ZoneScopedNC("Dockforge::shadow_cascades", 0xB8860B);
    TracyMessage("CSM invalidated", 15);
    const VkExtent2D shadow_extent{
        .width = shadow_map_cascade_resolution,
        .height = shadow_map_cascade_resolution,
    };
    const VkViewport shadow_viewport{
        .x = 0.F,
        .y = 0.F,
        .width = static_cast<float>(shadow_map_cascade_resolution),
        .height = static_cast<float>(shadow_map_cascade_resolution),
        .minDepth = 0.F,
        .maxDepth = 1.F,
    };
    const VkRect2D shadow_scissor{
        .offset =
            {
                .x = 0,
                .y = 0,
            },
        .extent = shadow_extent,
    };

    const auto &pipeline =
        renderer->pipeline_registry->get(renderer->shadow_pipeline);
    vkCmdBindPipeline(ctx.main_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(ctx.main_cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            renderer->pipeline_layout, 0U, 1u,
                            &renderer->bindless.set, 0u, nullptr);
    vkCmdBindIndexBuffer(ctx.main_cb,
                         renderer->geometry_pool->index_buffer->get_buffer(),
                         0u, VK_INDEX_TYPE_UINT32);
    for (u32 cascade_idx = 0U; cascade_idx < shadow_map_cascade_count;
         ++cascade_idx) {
      VkRenderingAttachmentInfo depth_att{};
      depth_att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
      depth_att.imageView = renderer->csm.layer_views[cascade_idx];
      depth_att.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
      depth_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
      depth_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      depth_att.clearValue = {
          .depthStencil =
              {
                  .depth = 1.F,
                  .stencil = 0U,
              },
      };
      VkRenderingInfo ri{};
      ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
      ri.renderArea = shadow_scissor;
      ri.layerCount = 1U;
      ri.pDepthAttachment = &depth_att;

      vkCmdBeginRendering(ctx.main_cb, &ri);
      vkCmdSetViewport(ctx.main_cb, 0u, 1u, &shadow_viewport);
      vkCmdSetScissor(ctx.main_cb, 0u, 1u, &shadow_scissor);
      vkCmdSetCullMode(ctx.main_cb, VK_CULL_MODE_FRONT_BIT);
      vkCmdSetDepthBias(ctx.main_cb, 1.25f, 0.F, 1.75f);

      renderer->render_shadow_cascade(ctx.main_cb, cascade_idx);

      vkCmdEndRendering(ctx.main_cb);
      shadow_map_state.invalid = false;
    }

    VkImageMemoryBarrier2 csm_to_sampled{};
    csm_to_sampled.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    csm_to_sampled.srcStageMask = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    csm_to_sampled.srcAccessMask =
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    csm_to_sampled.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    csm_to_sampled.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    csm_to_sampled.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    csm_to_sampled.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    csm_to_sampled.image = renderer->csm.image;
    csm_to_sampled.subresourceRange = {
        .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = shadow_map_cascade_count,
    };
    emit_barrier(ctx.main_cb, csm_to_sampled);
  }
  {
    ZoneScopedNC("Dockforge::depth_prepass", 0xFF11AA);
    const auto &resolve_target =
        renderer->resolve(viewport_resources.depth_resolved_target);
    VkRenderingAttachmentInfo depth_attachment{};
    depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth_attachment.imageView = viewport_resources.depth_msaa.sampled_view;
    depth_attachment.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth_attachment.resolveMode = VK_RESOLVE_MODE_MIN_BIT;
    depth_attachment.resolveImageView = resolve_target.sampled_view;
    depth_attachment.resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL;
    depth_attachment.clearValue = {
        .depthStencil =
            {
                .depth = 0.F,
                .stencil = 0U,
            },
    };
    VkRenderingInfo prepass_ri{};
    prepass_ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    prepass_ri.renderArea = scissor;
    prepass_ri.layerCount = 1u;
    prepass_ri.pDepthAttachment = &depth_attachment;

    vkCmdBeginRendering(ctx.main_cb, &prepass_ri);
    vkCmdSetViewport(ctx.main_cb, 0u, 1u, &viewport);
    vkCmdSetScissor(ctx.main_cb, 0u, 1u, &scissor);
    vkCmdSetCullMode(ctx.main_cb, VK_CULL_MODE_BACK_BIT);
    vkCmdSetFrontFace(ctx.main_cb, VK_FRONT_FACE_CLOCKWISE);
    vkCmdSetDepthCompareOp(ctx.main_cb, VK_COMPARE_OP_GREATER_OR_EQUAL);

    renderer->render_pass(ctx.main_cb, renderer->depth_prepass,
                          renderer->pipeline_registry->get(depth_pipeline));
    vkCmdEndRendering(ctx.main_cb);

    VkImageMemoryBarrier2 depth_barrier{};
    depth_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    depth_barrier.srcStageMask = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    depth_barrier.srcAccessMask =
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    depth_barrier.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
    depth_barrier.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    depth_barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    depth_barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    depth_barrier.image = viewport_resources.depth_msaa.image;
    depth_barrier.subresourceRange = {
        .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
        .levelCount = 1u,
        .layerCount = 1u,
    };
    emit_barrier(ctx.main_cb, depth_barrier);
  }

  // Occlusion culling pass
  {
    ZoneScopedNC("Dockforge::hiz_build", 0xDDA0DD);
    renderer->blit_depth_to_pre_hiz_pass(
        ctx.main_cb, viewport_resources.depth_resolved_target,
        viewport_resources.depth_pre_hiz);
    renderer->build_hierarchical_depth_pyramid_pass(
        ctx.main_cb, viewport_resources.depth_pre_hiz,
        viewport_resources.hierarchical_depth_pyramid_target);

    const auto &resolve_target =
        renderer->resolve(viewport_resources.hierarchical_depth_pyramid_target);
    VkImageMemoryBarrier2 hiz_barrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .image = resolve_target.image,
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .levelCount = VK_REMAINING_MIP_LEVELS,
                .layerCount = 1,
            },
    };
    emit_barrier(ctx.main_cb, hiz_barrier);
  }
  {
    renderer->forward_occlusion_culling_pass(
        ctx.main_cb, viewport_resources.hierarchical_depth_pyramid_target);
  }
  {
    renderer->light_clustering_pass(ctx.main_cb);
  }
  {
    ZoneScopedNC("Dockforge::geometry_msaa_pass", 0xAA11FF);

    VkRenderingAttachmentInfo forward_color{};
    forward_color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    forward_color.imageView =
        viewport_resources.forward_target_msaa.sampled_view;
    forward_color.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    forward_color.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
    forward_color.resolveImageView = forward_texture.sampled_view;
    forward_color.resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL;
    forward_color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    forward_color.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    forward_color.clearValue = {.color = {{0.F, 0.F, 0.F, 0.F}}};
    VkRenderingAttachmentInfo forward_depth{};
    forward_depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    forward_depth.imageView = viewport_resources.depth_msaa.sampled_view;
    forward_depth.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    forward_depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    forward_depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    VkRenderingInfo forward_ri{};
    forward_ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    forward_ri.renderArea = scissor;
    forward_ri.layerCount = 1u;
    forward_ri.colorAttachmentCount = 1u;
    forward_ri.pColorAttachments = &forward_color;
    forward_ri.pDepthAttachment = &forward_depth;

    vkCmdBeginRendering(ctx.main_cb, &forward_ri);
    vkCmdSetViewport(ctx.main_cb, 0u, 1u, &viewport);
    vkCmdSetScissor(ctx.main_cb, 0u, 1u, &scissor);
    {
      vkCmdSetDepthWriteEnable(ctx.main_cb, VK_FALSE);
      vkCmdSetDepthTestEnable(ctx.main_cb, VK_FALSE);
      vkCmdSetDepthCompareOp(ctx.main_cb, VK_COMPARE_OP_EQUAL);
      vkCmdSetCullMode(ctx.main_cb, VK_CULL_MODE_NONE);
      renderer->skybox_pass(ctx.main_cb);
    }

    vkCmdSetCullMode(ctx.main_cb, VK_CULL_MODE_BACK_BIT);
    vkCmdSetDepthCompareOp(ctx.main_cb, VK_COMPARE_OP_EQUAL);
    vkCmdSetDepthWriteEnable(ctx.main_cb, VK_FALSE);
    vkCmdSetDepthTestEnable(ctx.main_cb, VK_TRUE);
    vkCmdSetFrontFace(ctx.main_cb, VK_FRONT_FACE_CLOCKWISE);
    vkCmdBindIndexBuffer(ctx.main_cb,
                         renderer->geometry_pool->index_buffer->get_buffer(),
                         0u, VK_INDEX_TYPE_UINT32);
    renderer->render_pass(ctx.main_cb, renderer->forward_pass);

    canvas_renderer->render(ctx.main_cb, projection * view,
                            std::make_tuple(VK_FORMAT_R16G16B16A16_SFLOAT,
                                            VK_FORMAT_D32_SFLOAT, viewport,
                                            scissor));

    vkCmdEndRendering(ctx.main_cb);

    VkImageMemoryBarrier2 forward_to_composite{};
    forward_to_composite.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    forward_to_composite.srcStageMask =
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    forward_to_composite.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    forward_to_composite.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    forward_to_composite.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    forward_to_composite.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    forward_to_composite.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    forward_to_composite.image =
        forward_texture.image; // resolved target, not MSAA
    forward_to_composite.subresourceRange = color_range;
    emit_barrier(ctx.main_cb, forward_to_composite);
  }
  {
    ZoneScopedNC("Dockforge::tonemap_pass", 0xFFA07A);
    VkRenderingAttachmentInfo display_color{};
    display_color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    display_color.imageView = display_texture.sampled_view;
    display_color.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    display_color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    display_color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo composite_ri{};
    composite_ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    composite_ri.renderArea = scissor;
    composite_ri.layerCount = 1U;
    composite_ri.colorAttachmentCount = 1U;
    composite_ri.pColorAttachments = &display_color,

    vkCmdBeginRendering(ctx.main_cb, &composite_ri);
    renderer->composite_pass(ctx.main_cb);
    vkCmdEndRendering(ctx.main_cb);

    VkImageMemoryBarrier2 composite_to_imgui{};
    composite_to_imgui.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;

    composite_to_imgui.srcStageMask =
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    composite_to_imgui.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    composite_to_imgui.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    composite_to_imgui.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    composite_to_imgui.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    composite_to_imgui.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    composite_to_imgui.image = display_texture.image;
    composite_to_imgui.subresourceRange = color_range;
    emit_barrier(ctx.main_cb, composite_to_imgui);
  }
  {
    ZoneScopedNC("Dockforge::imgui_pass", 0xFFA500);
    const std::array<VkImageMemoryBarrier2, 1> swapchain_barriers{
        VkImageMemoryBarrier2{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .pNext = nullptr,
            .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask = 0,
            .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = ctx.swapchain_image.image,
            .subresourceRange = color_range,
        },
    };
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1U;
    dep.pImageMemoryBarriers = swapchain_barriers.data();
    vkCmdPipelineBarrier2(ctx.main_cb, &dep);

    VkRenderingAttachmentInfo swapchain_color{};
    swapchain_color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    swapchain_color.imageView = ctx.swapchain_image.view;
    swapchain_color.imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL;
    swapchain_color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    swapchain_color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo swapchain_ri{};
    swapchain_ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    swapchain_ri.renderArea =
        {
            .offset = {.x = 0, .y = 0},
            .extent = ctx.swapchain_image.extent,
        },
    swapchain_ri.layerCount = 1U;
    swapchain_ri.colorAttachmentCount = 1U;
    swapchain_ri.pColorAttachments = &swapchain_color;

    vkCmdBeginRendering(ctx.main_cb, &swapchain_ri);
    imgui_renderer->render(ctx.main_cb);
    vkCmdEndRendering(ctx.main_cb);
  }
  {
    VkImageMemoryBarrier2 present_barrier{};
    present_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    present_barrier.srcStageMask =
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    present_barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    present_barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    present_barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    present_barrier.image = ctx.swapchain_image.image;
    present_barrier.subresourceRange = color_range;
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1U;
    dep.pImageMemoryBarriers = &present_barrier;
    vkCmdPipelineBarrier2(ctx.main_cb, &dep);
  }

  return ctx.next_frame_wait_value();
}

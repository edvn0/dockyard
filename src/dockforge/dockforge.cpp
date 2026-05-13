#include <dockforge/dockforge.hpp>

#include <dockforge/editor_camera.hpp>
#include <dockforge/editor_utils.hpp>

#include <dockyard/imgui_renderer.hpp>
#include <dockyard/scene_renderer.hpp>

#include <GLFW/glfw3.h>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <imgui.h>

#include <ImGuizmo.h>

#include "./cube_vertices.inl"

auto make_app() -> std::unique_ptr<Dockforge> {
  return std::make_unique<Dockforge>();
}

Dockforge::~Dockforge() = default;

auto Dockforge::init(const InitialisationContext &ctx) -> void {
  renderer =
      std::make_unique<SceneRenderer>(ctx.context, ctx.swapchain_resources);
  context = &ctx.context;

  editor_scene = std::make_shared<Scene>();
  active_scene = editor_scene;
  active_scene->on_construct<Components::Tag>()
      .connect<&Dockforge::on_changed_tag>(*this);
  active_scene->on_destroy<Components::Tag>()
      .connect<&Dockforge::on_changed_tag>(*this);

  auto &&[w, h] = ctx.window_extent;

  editor_camera =
      std::make_unique<EditorCamera>(get_window(), glm::vec3{0.0f, 5.0f, -6.0f},
                                     glm::vec3{0.0f, 0.0f, 0.0f}, w, h);

  {
    const u32 white = 0xFFFFFFFF;
    const u32 blue = glm::packUnorm4x8(glm::vec4(0.5f, 0.5f, 1.0f, 1.0f));
    const u32 mr_default = glm::packUnorm4x8(glm::vec4(0.0f, 1.0f, 0.0f, 1.0f));
    const u32 occlusion_default = 0xFFFFFFFF;
    const u32 black = 0xFF000000;

    auto white_texture =
        renderer->upload_texture(std::span(&white, 1), "white_fallback_texture",
                                 1, 1, VK_FORMAT_R8G8B8A8_UNORM, false);
    (void)renderer->upload_texture(std::span(&blue, 1),
                                   "normal_fallback_texture", 1, 1,
                                   VK_FORMAT_R8G8B8A8_UNORM, false);
    (void)renderer->upload_texture(std::span(&mr_default, 1),
                                   "metallic_roughness_fallback_texture", 1, 1,
                                   VK_FORMAT_R8G8B8A8_UNORM, false);
    (void)renderer->upload_texture(std::span(&occlusion_default, 1),
                                   "occlusion_fallback_texture", 1, 1,
                                   VK_FORMAT_R8G8B8A8_UNORM, false);
    (void)renderer->upload_texture(std::span(&black, 1),
                                   "black_fallback_texture", 1, 1,
                                   VK_FORMAT_R8G8B8A8_UNORM, false);
    assert(white_texture.index() == 0);
    info("White texture index: {}", white_texture.index());

    imgui_renderer = std::make_unique<ImGuiRenderer>(
        get_window(), 16, *renderer,
        FontChoice{
            .font_path = VFSPath::create("fonts://RobotoMono-Regular.ttf"),
            .size = 15.0f,
        });
    imgui_renderer->set_app_name("Dockforge");

    renderer->initialise_bindless(white_texture);
    viewport_resources = ViewportResources::create(*context, *renderer, w, h);
    renderer->update_output_texture(viewport_resources.forward_target);
    renderer->bindless.need_repopulate = true;
  }

  {
    mesh_handle =
        mesh::load_from_memory(*renderer, cube_verts, cube_indices).value();

    auto &scene = *active_scene;
    const int grid_side = 3;
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
          entity.emplace<Components::Mesh>(mesh_handle);
        }
      }
    }
  }

  auto &registry = *renderer->pipeline_registry;

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

  renderer->bindless.repopulate_if_needed(
      renderer->textures, renderer->samplers, renderer->comparison_samplers);

  auto loaded = mesh::load_from_path(
      VFSPath::create("meshes://DamagedHelmet.glb"), *renderer);
  if (!loaded) {
    error("Could not load {}", "meshes://DamagedHelmet.glb");
    std::abort();
  }

  // Does not exist
  auto entity = active_scene->make("Helmet");
  entity.emplace<Components::Mesh>(*loaded);
  entity.get<Components::Transform>().position = {-5, 3, 9};
}
auto Dockforge::on_mouse_moved(const events::mouse_moved &e) -> void {
  if (glfwGetMouseButton(get_window(), GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS)
    editor_camera->on_mouse_delta(e.dx, e.dy);
}
auto Dockforge::on_key_released(const events::key_released &e) -> void {
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
[[nodiscard]] auto Dockforge::resolve_camera() const
    -> std::pair<glm::mat4, glm::mat4> {
  if (auto *cam = active_scene->primary_camera())
    return {cam->view(), cam->projection()};
  return {editor_camera->view(), editor_camera->projection()};
}
auto Dockforge::resize(u32 w, u32 h) -> void {
  info("Dockforge resized to {}x{}", w, h);
  viewport_resources.resize(*context, *renderer, w, h);
  renderer->resize();
  editor_camera->set_aspect(w, h);
  for (auto &&[e, cam] : active_scene->view<Components::Camera>().each())
    cam.set_aspect(w, h);
}
auto Dockforge::try_pick_entity(glm::vec2 mouse_screen) -> void {
  auto [view, proj] = resolve_camera();
  const auto ray = screen_to_ray(
      mouse_screen,
      glm::vec2(viewport_panel_offset.width, viewport_panel_offset.height),
      glm::vec2(viewport_panel_extent.width, viewport_panel_extent.height),
      view, proj);

  entt::entity best = entt::null;
  float best_t = std::numeric_limits<float>::max();

  for (auto &&[e, xt, m] :
       active_scene->group<Components::Transform, Components::Mesh>().each()) {
    const MeshAsset *asset = renderer->get_mesh(m);
    if (!asset || !asset->mesh_aabb.is_valid())
      continue;

    const AABB world_aabb = asset->mesh_aabb.transform(xt.matrix());
    const float t = ray_aabb(ray, world_aabb.get_min(), world_aabb.get_max());

    if (t >= 0.0f && t < best_t) {
      best_t = t;
      best = e;
    }
  }

  state.selected = best;
}

void Dockforge::refresh_entity_cache() {
  state.entity_cache.clear();
  auto tag_view = active_scene->view<Components::Tag>();
  for (auto e : tag_view) {
    state.entity_cache.push_back(e);
  }
  state.cache_dirty = false;
}

void Dockforge::draw_scene_outliner() {
  if (state.cache_dirty) {
    refresh_entity_cache();
  }

  if (!ImGui::Begin("Entities")) {
    ImGui::End();
    return;
  }

  // Optional: filter input
  static ImGuiTextFilter filter;
  filter.Draw("##filter", -1.0f);

  ImGui::Separator();

  const float item_height = ImGui::GetTextLineHeightWithSpacing();
  ImGui::BeginChild("##entity_list", ImVec2(0, 0), false,
                    ImGuiWindowFlags_HorizontalScrollbar);

  ImGuiListClipper clipper;
  clipper.Begin(static_cast<int>(state.entity_cache.size()), item_height);

  while (clipper.Step()) {
    for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
      auto e = state.entity_cache[static_cast<usize>(i)];
      Entity entity{*active_scene, e};

      auto *tag = entity.try_get<Components::Tag>();
      auto *mesh = entity.try_get<Components::Mesh>();

      if (!tag)
        continue;

      const std::string_view label = tag->tag;
      if (!filter.PassFilter(label.data()))
        continue;

      // Build a display string without allocating when possible
      char row_label[128];
      if (mesh) {
        std::snprintf(row_label, sizeof(row_label), "[M] %.*s  (%u)##%u",
                      static_cast<int>(label.size()), label.data(),
                      mesh->handle.index(), static_cast<uint32_t>(e));
      } else {
        std::snprintf(row_label, sizeof(row_label), "     %.*s##%u",
                      static_cast<int>(label.size()), label.data(),
                      static_cast<uint32_t>(e));
      }

      const bool is_selected = (state.selected == e);
      if (ImGui::Selectable(row_label, is_selected,
                            ImGuiSelectableFlags_SpanAllColumns)) {
        state.selected = e;
      }

      if (is_selected) {
        ImGui::SetItemDefaultFocus();
      }
    }
  }

  clipper.End();
  ImGui::EndChild();
  ImGui::End();
}
auto Dockforge::build_ui() -> void {
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
  ImGui::DockSpace(dockspace_id, {0.0f, 0.0f},
                   ImGuiDockNodeFlags_PassthruCentralNode);
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

  const auto viewport_screen_pos = ImGui::GetCursorScreenPos();

  ImGui::Image(ImTextureRef{ImTextureID{
                   viewport_resources.display_target.index(),
               }},
               panel_size);

  ImGuizmo::SetDrawlist();
  ImGuizmo::SetOrthographic(false);
  ImGuizmo::SetRect(viewport_screen_pos.x, viewport_screen_pos.y, panel_size.x,
                    panel_size.y);
  const bool viewport_hovered = ImGui::IsWindowHovered();
  const bool viewport_focused = ImGui::IsWindowFocused();
  const bool hovered = ImGui::IsItemHovered();
  if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
      !ImGuizmo::IsOver()) {
    const ImVec2 mp = ImGui::GetMousePos();
    pending_pick = glm::vec2(mp.x, mp.y);
  }
  if (state.selected != entt::null) {
    Entity entity{*active_scene, state.selected};
    if (auto *xt = entity.try_get<Components::Transform>()) {
      auto [view, proj] = resolve_camera();

      glm::mat4 model = xt->matrix();

      ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj),
                           gizmo_op, // see field below
                           ImGuizmo::LOCAL, glm::value_ptr(model));

      if (ImGuizmo::IsUsing()) {
        glm::vec3 translation, scale, skew;
        glm::vec4 perspective;
        glm::quat rotation;
        glm::decompose(model, scale, rotation, translation, skew, perspective);
        xt->position = translation;
        xt->rotation = glm::normalize(rotation);
        xt->scale = scale;
      }
    }
  }
  ImGui::End();

  ImGui::ShowDemoWindow();

  if (ImGui::Begin("test")) {
    ImGui::Text("%d x %d", viewport_panel_extent.width,
                viewport_panel_extent.height);
    ImGui::Text("%d x %d", viewport_panel_offset.width,
                viewport_panel_offset.height);
    ImGui::End();
  }

  draw_scene_outliner();

  if (viewport_hovered || viewport_focused) {
    ImGui::GetIO().WantCaptureMouse = false;
    ImGui::GetIO().WantCaptureKeyboard = false;
  }
}
auto Dockforge::destroy() -> void {
  imgui_renderer.reset();

  viewport_resources.destroy(*context);
  renderer->destroy();

  editor_camera.reset();
  editor_scene.reset();
  runtime_scene.reset();
  active_scene.reset();
  renderer.reset();
}
auto Dockforge::update(float ts) -> void {
  if (!active_scene->primary_camera())
    editor_camera->update(ts);

  for (auto &&[e, xt, m] :
       active_scene->group<Components::Transform, Components::Mesh>().each()) {
    const float unique_seed =
        static_cast<float>(static_cast<u32>(e) % 100u) / 100.0f;
    const glm::vec3 local_axis =
        glm::normalize(glm::vec3(unique_seed, 1.0f, 0.5f));
    xt.rotation =
        glm::normalize(xt.rotation * glm::angleAxis(ts * 2.0f, local_axis));
  }
}
auto Dockforge::render(RenderContext &ctx) -> u64 {
  if (pending_pick) {
    try_pick_entity(*pending_pick);
    pending_pick.reset();
  }

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
  for (auto &&[e, xt, m] :
       active_scene->group<Components::Transform, Components::Mesh>().each()) {
    renderer->submit(m.handle, xt, forward_pipeline.get(), default_material);
  }

  auto [view, projection] = resolve_camera();
  renderer->prepare(ctx.frame_index, view, projection);

  if (renderer->bindless.repopulate_if_needed(renderer->textures,
                                              renderer->samplers,
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
                               .pImageMemoryBarriers = initial_barriers.data()};
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
    vkCmdBindIndexBuffer(ctx.main_cb,
                         renderer->geometry_pool->index_buffer->get_buffer(),
                         0u, VK_INDEX_TYPE_UINT32);
    renderer->render_pass(ctx.main_cb, ctx.frame_index,
                          *renderer->geometry_pool, renderer->depth_prepass,
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
                               .pImageMemoryBarriers = &forward_target_barrier};
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
    vkCmdBindIndexBuffer(ctx.main_cb,
                         renderer->geometry_pool->index_buffer->get_buffer(),
                         0u, VK_INDEX_TYPE_UINT32);
    renderer->render_pass(ctx.main_cb, ctx.frame_index,
                          *renderer->geometry_pool, renderer->forward_pass);
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

#include <dockforge/dockforge.hpp>

#include <dockforge/editor_camera.hpp>
#include <dockforge/editor_utils.hpp>
#include <dockforge/matrix_cache.hpp>

#include <dockyard/buffer.hpp>
#include <dockyard/components.hpp>
#include <dockyard/imgui_renderer.hpp>
#include <dockyard/mesh_loader.hpp>
#include <dockyard/scene.hpp>
#include <dockyard/scene_renderer.hpp>

#include <GLFW/glfw3.h>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <imgui.h>

#include <ImGuizmo.h>

#include "./cube_vertices.inl"

namespace {
auto make_default_override_materials(u32 count) -> std::vector<GPUMaterial> {
  std::vector<GPUMaterial> output(count);
  for (auto &material : output) {
    material.albedo_factor[0] = material.albedo_factor[1] =
        material.albedo_factor[2] = material.albedo_factor[3] = 1.0F;
    material.roughness_factor = 1.0F;
    material.normal_scale = 1.0F;
    material.occlusion_strength = 1.0F;
    material.albedo_index = 0U; // white fallback
  }
  return output;
}
} // namespace

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
            .size = 15.0F,
        });
    imgui_renderer->set_app_name("Dockforge");

    canvas_renderer = std::make_unique<CanvasRenderer>(*renderer);

    viewport_resources = ViewportResources::create(*context, *renderer, w, h);
    renderer->update_output_texture(viewport_resources.forward_target);
    renderer->bindless.need_repopulate = true;
    renderer->initialise_bindless(white_texture);

    constexpr u32 override_material_count_initial = 16U;
    auto blank =
        make_default_override_materials(override_material_count_initial);
    auto offset = renderer->geometry_pool->allocate_materials(std::span(blank));
    override_pool = {
        .base_slot = offset.start_index,
        .capacity = override_material_count_initial,
    };
  }

  {
    mesh_handle =
        mesh::load_from_memory(*renderer, cube_verts, cube_indices).value();

    auto &scene = *active_scene;
    const int grid_side = 3;
    const float spacing = std::numbers::sqrt2_v<float> + 0.5F;
    const float offset = (grid_side - 1) * spacing / 2.0F;

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
    auto floor = scene.make("Floor");
    floor.emplace<Components::Mesh>(mesh_handle);
    floor.get<Components::Transform>().scale = {30, 1, 30};
    floor.get<Components::Transform>().position = {0, -10, 0};
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
  if (loaded) {
    auto entity = active_scene->make("Helmet");
    entity.emplace<Components::Mesh>(*loaded);
    entity.get<Components::Transform>().position = {-5, 3, 9};
  }

  if (auto loaded_sponza = mesh::load_from_path(
          VFSPath::create("meshes://main_sponza.glb"), *renderer)) {
    auto sponza = active_scene->make("Sponza");
    sponza.emplace<Components::Mesh>(*loaded_sponza);
    sponza.get<Components::Transform>().position = {-10, 3, 9};
  }

  auto frustum_entity = active_scene->make("DebugFrustum");
  auto &df = frustum_entity.emplace<Components::DebugFrustum>();
  df.view = glm::lookAtLH(glm::vec3{5, -5, -10}, glm::vec3{0, 0, 0},
                          glm::vec3{0, 1, 0});
  df.proj = glm::perspective(glm::radians(30.0f), 1.77f, 0.1f, 30.0f);
  df.color = glm::vec4{1.0f, 1.0f, 0.0f, 1.0f};

  initialise_matrix_cache(get_frame_index_mut());
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
[[nodiscard]] auto Dockforge::resolve_camera_with_position() const
    -> std::tuple<glm::mat4, glm::mat4, glm::vec3> {
  if (auto *cam = active_scene->primary_camera())
    return {cam->view(), cam->projection(), cam->position};
  return {editor_camera->view(), editor_camera->projection(),
          editor_camera->position()};
}
auto Dockforge::resize(u32 w, u32 h) -> void {
  trace("Dockforge resized to {}x{}", w, h);
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
       active_scene->view<Components::Transform, Components::Mesh>().each()) {
    const MeshAsset *asset = renderer->get_mesh(m);
    if ((asset == nullptr) || !asset->mesh_aabb.is_valid())
      continue;

    const AABB world_aabb = asset->mesh_aabb.transform(xt.matrix());
    const float t = ray_aabb(ray, world_aabb.get_min(), world_aabb.get_max());

    if (t >= 0.0F && t < best_t) {
      best_t = t;
      best = e;
    }
  }

  state.selected = best;
  gizmo_prev_model.reset();
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
  ImGui::BeginChild("##entity_list", ImVec2(0, 0), ImGuiChildFlags_None,
                    ImGuiWindowFlags_HorizontalScrollbar);

  ImGuiListClipper clipper;
  clipper.Begin(static_cast<int>(state.entity_cache.size()), item_height);

  while (clipper.Step()) {
    for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
      auto e = state.entity_cache[static_cast<usize>(i)];
      Entity entity{*active_scene, e};

      auto *tag = entity.try_get<Components::Tag>();
      auto *mesh = entity.try_get<Components::Mesh>();

      if (tag == nullptr)
        continue;

      const std::string_view label = tag->tag;
      if (!filter.PassFilter(label.data(), std::cend(label)))
        continue;

      // Build a display string without allocating when possible
      std::array<char, 128> row_label{};
      if (mesh != nullptr) {
        std::snprintf(row_label.data(), std::size(row_label),
                      "[M] %.*s  (%u)##%u", static_cast<int>(label.size()),
                      label.data(), mesh->handle.index(),
                      static_cast<uint32_t>(e));
      } else {
        std::snprintf(row_label.data(), std::size(row_label), "     %.*s##%u",
                      static_cast<int>(label.size()), label.data(),
                      static_cast<uint32_t>(e));
      }

      const bool is_selected = (state.selected == e);
      if (ImGui::Selectable(row_label.data(), is_selected,
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

[[nodiscard]] auto draw_material_editor(GPUMaterial &mat) -> bool {
  bool changed = false;
  changed |= ImGui::ColorEdit4("Albedo Factor", mat.albedo_factor);
  changed |= ImGui::ColorEdit3("Emissive Factor", mat.emissive_factor);
  ImGui::SameLine();
  changed |= ImGui::SliderFloat("Emissive Factor", &mat.emissive_factor[3],
                                0.0F, 10.0F);
  ImGui::NewLine();

  changed |= ImGui::SliderFloat("Metallic", &mat.metallic_factor, 0.0F, 1.0F);
  changed |= ImGui::SliderFloat("Roughness", &mat.roughness_factor, 0.0F, 1.0F);
  changed |= ImGui::SliderFloat("Normal Scale", &mat.normal_scale, 0.0F, 2.0F);
  changed |= ImGui::SliderFloat("Occlusion Strength", &mat.occlusion_strength,
                                0.0F, 1.0F);
  changed |= ImGui::SliderFloat("Alpha Cutoff", &mat.alpha_cutoff, 0.0F, 1.0F);

  static constexpr const char *alpha_modes[] = {"Opaque", "Mask", "Blend"};
  int alpha_mode = static_cast<int>(mat.alpha_mode);
  if (ImGui::Combo("Alpha Mode", &alpha_mode, alpha_modes, 3)) {
    mat.alpha_mode = static_cast<u32>(alpha_mode);
    changed = true;
  }

  ImGui::SeparatorText("Texture Slots");
  ImGui::BeginDisabled();

  auto texture_row = [](const char *label, u32 index) {
    static constexpr u32 k_none = ~0u;
    const float icon_size = ImGui::GetFrameHeight();
    const ImVec2 icon_dim{icon_size, icon_size};
    const ImVec2 p = ImGui::GetCursorScreenPos();
    auto *dl = ImGui::GetWindowDrawList();

    if (index != k_none) {
      ImGui::Image(ImTextureRef{ImTextureID{index}}, icon_dim, {0, 0}, {1, 1},
                   {1, 1, 1, 1}, ImGui::GetStyleColorVec4(ImGuiCol_Border));
    } else {
      // 2×2 checkerboard placeholder
      const float h = icon_size * 0.5f;
      dl->AddRectFilled(p, {p.x + icon_size, p.y + icon_size},
                        IM_COL32(40, 40, 40, 255));
      dl->AddRectFilled({p.x, p.y}, {p.x + h, p.y + h},
                        IM_COL32(65, 65, 65, 255));
      dl->AddRectFilled({p.x + h, p.y + h}, {p.x + icon_size, p.y + icon_size},
                        IM_COL32(65, 65, 65, 255));
      dl->AddRect(p, {p.x + icon_size, p.y + icon_size},
                  ImGui::GetColorU32(ImGuiCol_Border));
      ImGui::Dummy(icon_dim);
    }

    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    // vertically centre the InputInt against the icon
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() +
                         (icon_size - ImGui::GetFrameHeight()) * 0.5f);
    int idx = static_cast<int>(index);
    ImGui::InputInt(label, &idx, 0, 0, ImGuiInputTextFlags_ReadOnly);
  };

  texture_row("Albedo", mat.albedo_index);
  texture_row("Normal", mat.normal_index);
  texture_row("Metallic/Roughness", mat.metallic_roughness_index);
  texture_row("Emissive", mat.emissive_index);
  texture_row("Occlusion", mat.occlusion_index);

  ImGui::EndDisabled();
  return changed;
}

auto Dockforge::remove_override(Entity entity) -> void {
  auto *material_override = entity.try_get<Components::MaterialOverride>();
  if (material_override == nullptr)
    return;

  if (material_override->gpu_slot != ~0U)
    override_pool.free(material_override->gpu_slot);

  entity.remove<Components::MaterialOverride>();
}

auto Dockforge::draw_debug_shapes() -> void {
  for (auto &&[e, line] : active_scene->view<Components::DebugLine>().each()) {
    canvas_renderer->line(line.p1, line.p2, line.color);
  }

  for (auto &&[e, xt, box] :
       active_scene->view<Components::Transform, Components::DebugBox>()
           .each()) {
    canvas_renderer->box(cached_matrix(e, xt),
                         box.size * 0.5F, // canvas API takes half-extents
                         box.color);
  }

  for (auto &&[e, xt, plane] :
       active_scene->view<Components::Transform, Components::DebugPlane>()
           .each()) {
    const glm::vec3 origin = glm::vec3(cached_matrix(e, xt)[3]);
    canvas_renderer->plane(origin, plane.v1, plane.v2, plane.n1, plane.n2,
                           plane.s1, plane.s2, plane.color, plane.outline);
  }

  for (auto &&[e, frustum] :
       active_scene->view<Components::DebugFrustum>().each()) {
    canvas_renderer->frustum(frustum.view, frustum.proj, frustum.color);
  }
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
      last_ui_size = {.width = panel_w, .height = panel_h};
      last_ui_offset = {
          .width = static_cast<u32>(ImGui::GetCursorScreenPos().x),
          .height = static_cast<u32>(ImGui::GetCursorScreenPos().y),
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

  /*
  auto [view, proj] = resolve_camera();
  glm::mat4 view_matrix = view; // local copy so ViewManipulate can write back

  ImVec2 gizmo_size = {128.f, 128.f};
  ImVec2 gizmo_pos = {viewport_screen_pos.x + panel_size.x - gizmo_size.x,
                      viewport_screen_pos.y};

  ImGuizmo::ViewManipulate(glm::value_ptr(view_matrix), 8.f, gizmo_pos,
                           gizmo_size, 0x10101010);

  if (std::memcmp(glm::value_ptr(view), glm::value_ptr(view_matrix),
                  sizeof(glm::mat4)) != 0) {
    const glm::mat3 rot = glm::mat3(view_matrix);
    const glm::vec3 trans = glm::vec3(view_matrix[3]);
    const glm::vec3 new_pos = -glm::transpose(rot) * trans;
    // Third row of view matrix is -forward in LH
    const glm::vec3 forward =
        -glm::vec3(view_matrix[0][2], view_matrix[1][2], view_matrix[2][2]);
    editor_camera->set_pose(new_pos, new_pos + forward);
  }
    */

  constexpr auto remove_rotation = [](const auto &m) {
    glm::mat4 result(1.0f);

    // Extract scale by getting the length of the basis vectors
    result[0][0] = glm::length(glm::vec3(m[0]));
    result[1][1] = glm::length(glm::vec3(m[1]));
    result[2][2] = glm::length(glm::vec3(m[2]));

    // Copy the translation column
    result[3] = m[3];

    return result;
  };

  MutableMaterialView selected_entity_materials{};

  if (state.selected != entt::null) {
    Entity entity{*active_scene, state.selected};
    auto &transform = entity.get<Components::Transform>();
    auto &&[view, proj] = resolve_camera();

    glm::mat4 model =
        cached_matrix(state.selected, entity.get<Components::Transform>());

    ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj), gizmo_op,
                         ImGuizmo::LOCAL, glm::value_ptr(model));

    if (ImGuizmo::IsUsing()) {
      if (!gizmo_prev_model.has_value()) {
        gizmo_prev_model =
            cached_matrix(state.selected, entity.get<Components::Transform>());
      }

      const glm::mat4 delta = glm::inverse(*gizmo_prev_model) * model;
      glm::vec3 delta_translation;
      glm::vec3 delta_scale;
      glm::vec3 delta_skew;
      glm::vec4 delta_perspective;
      glm::quat delta_rotation;

      if (glm::decompose(delta, delta_scale, delta_rotation, delta_translation,
                         delta_skew, delta_perspective)) {
        transform.position += delta_translation;
        transform.rotation =
            glm::normalize(delta_rotation * transform.rotation);
        transform.scale *= delta_scale;
      }

      gizmo_prev_model = model;
    } else {
      gizmo_prev_model.reset();
    }

    if (auto *mesh = entity.try_get<Components::Mesh>()) {
      auto *resolved = renderer->get_mesh(*mesh);
      canvas_renderer->box(remove_rotation(model), resolved->mesh_aabb,
                           glm::vec4{0.1, 0.9, 0.2, 1.0F});

      selected_entity_materials = renderer->get_material_view_mut(*mesh);
    }

    if (ImGui::Begin("Materials")) {
      if (state.selected == entt::null) {
        ImGui::TextDisabled("No entity selected");
        ImGui::End();
      } else {
        Entity entity{*active_scene, state.selected};
        auto *mesh = entity.try_get<Components::Mesh>();

        if (auto *ov = entity.try_get<Components::MaterialOverride>()) {
          ImGui::TextDisabled("Override active");
          ImGui::SameLine();

          ImGui::PushStyleColor(ImGuiCol_Button,
                                ImVec4{0.55F, 0.15F, 0.15F, 1.0F});
          ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                ImVec4{0.75F, 0.20F, 0.20F, 1.0F});
          ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                ImVec4{0.90F, 0.25F, 0.25F, 1.0F});
          bool was_removed = false;
          if (ImGui::SmallButton("Remove Override")) {
            remove_override(entity);
            was_removed = true;
          }
          ImGui::PopStyleColor(3);

          if (!was_removed) {
            ImGui::Separator();
            ImGui::PushID("override");
            if (draw_material_editor(ov->material))
              ov->dirty = true;
            ImGui::PopID();
          }

        } else if (mesh != nullptr) {
          auto mats = renderer->get_material_view_mut(*mesh);

          if (mats.empty()) {
            ImGui::TextDisabled("Mesh has no materials");
          } else {
            for (u32 i = 0; i < mats.size(); ++i) {
              ImGui::PushID(static_cast<int>(i));
              if (ImGui::CollapsingHeader(
                      std::format("Material {}", i).c_str())) {
                if (draw_material_editor(mats[i]) &&
                    ImGui::IsItemDeactivatedAfterEdit())
                  renderer->geometry_pool->flush_material(mats.slot(i));
              }
              ImGui::PopID();
            }

            ImGui::Separator();
            if (ImGui::Button("Add Override")) {
              auto &new_ov = entity.emplace<Components::MaterialOverride>();
              new_ov.material = mats.first();
            }
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip("Stamps a single-material override.");
          }

        } else {
          ImGui::TextDisabled("No mesh component");
        }

        ImGui::End();
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

  draw_debug_shapes();
  canvas_renderer->render_2d();

  if (ImGui::Begin("Sun direction")) {
    glm::vec3 copy = renderer->sun_direction;
    if (ImGui::DragFloat3("Sun direction", glm::value_ptr(copy), 0.1F)) {
      renderer->sun_direction = glm::normalize(copy);
    }
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
  canvas_renderer.reset();

  viewport_resources.destroy(*context);
  renderer->destroy();

  editor_camera.reset();
  editor_scene.reset();
  runtime_scene.reset();
  active_scene.reset();
  renderer.reset();
}

auto Dockforge::update(float ts) -> void {
  if (active_scene->primary_camera() == nullptr)
    editor_camera->update(ts);
}

void emit_barrier(VkCommandBuffer cmd,
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

auto grow_pool(Dockforge &app) -> void {
  const u32 old_capacity = app.override_pool.capacity;
  const u32 new_capacity = old_capacity * 2;

  info("MaterialOverridePool growing {} → {} slots", old_capacity,
       new_capacity);

  vkDeviceWaitIdle(app.context->device);

  app.renderer->geometry_pool->reserve_materials(new_capacity - old_capacity);

  auto new_data = make_default_override_materials(new_capacity);

  if (app.override_pool.next > 0) {
    auto live = app.renderer->geometry_pool->get_materials(
        app.override_pool.base_slot, app.override_pool.next);
    std::ranges::reverse_copy(live, new_data.begin());
  }

  const auto new_offset =
      app.renderer->geometry_pool->allocate_materials(std::span(new_data));
  const u32 delta = new_offset.start_index - app.override_pool.base_slot;

  for (auto &&[e, ov] :
       app.active_scene->template view<Components::MaterialOverride>().each()) {
    if (ov.gpu_slot != ~0U)
      ov.gpu_slot += delta;
  }

  for (auto &s : app.override_pool.free_slots)
    s += delta;

  app.override_pool.base_slot = new_offset.start_index;
  app.override_pool.capacity = new_capacity;
  app.override_pool.needs_grow = false;
  app.renderer->bindless.need_repopulate = true;
}

auto Dockforge::resolve_material_slot(Entity e) -> u32 {
  constexpr auto default_material = 0U;

  auto *material_override = e.try_get<Components::MaterialOverride>();
  if (material_override == nullptr)
    return default_material;

  if (material_override->gpu_slot == ~0U) {
    if (auto slot = override_pool.alloc()) {
      material_override->gpu_slot = *slot;
      material_override->dirty = true;
    } else {
      warn("MaterialOverridePool full — override skipped this frame");
      return default_material;
    }
  }

  if (material_override->dirty) {
    renderer->geometry_pool->get_materials_mut(
        material_override->gpu_slot, 1)[0] = material_override->material;
    renderer->geometry_pool->flush_material(material_override->gpu_slot);
    material_override->dirty = false;
  }

  return material_override->gpu_slot;
}

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

auto Dockforge::render(RenderContext &ctx) -> u64 {
  if (pending_pick) [[unlikely]] {
    try_pick_entity(*pending_pick);
    pending_pick.reset();
  }

  if (override_pool.needs_grow) [[unlikely]] {
    grow_pool(*this);
  }

  const bool size_changed =
      (last_ui_size.width != viewport_panel_extent.width ||
       last_ui_size.height != viewport_panel_extent.height);
  if (size_changed) [[unlikely]] {
    resize_viewport(*this);
  }

  for (auto &&[e, xt, m] :
       active_scene->group<Components::Transform, Components::Mesh>().each()) {
    renderer->submit(m.handle, cached_matrix(e, xt), forward_pipeline.get(),
                     resolve_material_slot({*active_scene, e}));
  }
  auto [view, projection] = resolve_camera();
  renderer->update_csm(view, projection, editor_camera->near_plane(),
                       editor_camera->far_plane());
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

  const VkImageSubresourceRange color_range{
      .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
      .levelCount = 1U,
      .layerCount = 1U,
  };
  const VkImageSubresourceRange depth_range{
      .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
      .levelCount = 1U,
      .layerCount = 1U,
  };
  const VkExtent2D vp_extent = viewport_resources.extent();
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

  {
    // First frame: transition from UNDEFINED; subsequent: from GENERAL
    // (sampled)
    const VkImageMemoryBarrier2 csm_to_attachment{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                        VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        .dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL, // UNDEFINED on first frame
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .image = renderer->csm.image,
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = shadow_map_cascade_count,
            },
    };
    emit_barrier(ctx.main_cb, csm_to_attachment);

    const VkExtent2D shadow_extent{
        .width = shadow_map_cascade_resolution,
        .height = shadow_map_cascade_resolution,
    };
    const VkViewport shadow_viewport{
        .x = 0.0f,
        .y = 0.0f,
        .width = static_cast<float>(shadow_map_cascade_resolution),
        .height = static_cast<float>(shadow_map_cascade_resolution),
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    const VkRect2D shadow_scissor{
        .offset =
            {
                .x = 0,
                .y = 0,
            },
        .extent = shadow_extent,
    };

    for (u32 cascade_idx = 0U; cascade_idx < shadow_map_cascade_count;
         ++cascade_idx) {
      const VkRenderingAttachmentInfo depth_att{
          .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
          .imageView = renderer->csm.layer_views[cascade_idx],
          .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
          .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
          .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
          .clearValue =
              {
                  .depthStencil =
                      {
                          .depth = 1.0F,
                          .stencil = 0U,
                      },
              }, // conventional: 1=far
      };
      const VkRenderingInfo ri{
          .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
          .renderArea = shadow_scissor,
          .layerCount = 1U,
          .pDepthAttachment = &depth_att,
      };

      vkCmdBeginRendering(ctx.main_cb, &ri);
      vkCmdSetViewport(ctx.main_cb, 0u, 1u, &shadow_viewport);
      vkCmdSetScissor(ctx.main_cb, 0u, 1u, &shadow_scissor);
      vkCmdSetCullMode(ctx.main_cb, VK_CULL_MODE_FRONT_BIT);
      vkCmdSetDepthBias(ctx.main_cb, 1.25f, 0.0f, 1.75f);

      renderer->render_shadow_cascade(ctx.main_cb, cascade_idx);

      vkCmdEndRendering(ctx.main_cb);
    }

    const VkImageMemoryBarrier2 csm_to_sampled{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        .srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .image = renderer->csm.image,
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = shadow_map_cascade_count,
            },
    };
    emit_barrier(ctx.main_cb, csm_to_sampled);
  }

  // ── 2. Depth pre-pass ───────────────────────────────────────────────
  {
    const VkRenderingAttachmentInfo depth_attachment{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = viewport_resources.depth_msaa.sampled_view,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue =
            {
                .depthStencil =
                    {
                        .depth = 0.0f,
                        .stencil = 0u,
                    },
            },
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
    vkCmdSetDepthBias(ctx.main_cb, 1.25f, 0.0f, 1.75f);
    vkCmdBindIndexBuffer(ctx.main_cb,
                         renderer->geometry_pool->index_buffer->get_buffer(),
                         0u, VK_INDEX_TYPE_UINT32);
    renderer->render_pass(ctx.main_cb, renderer->depth_prepass,
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
    renderer->render_pass(ctx.main_cb, renderer->forward_pass);

    canvas_renderer->render(ctx.main_cb, projection * view,
                            std::make_tuple(VK_FORMAT_R16G16B16A16_SFLOAT,
                                            VK_FORMAT_D32_SFLOAT, viewport,
                                            scissor));

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
    const VkDependencyInfo dep{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1U,
        .pImageMemoryBarriers = &present_barrier,
    };
    vkCmdPipelineBarrier2(ctx.main_cb, &dep);
  }

  return ctx.next_frame_wait_value();
}

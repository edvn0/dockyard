#include <algorithm>
#include <dockforge/dockforge.hpp>

#include <dockforge/editor_camera.hpp>
#include <dockforge/editor_utils.hpp>
#include <dockforge/matrix_cache.hpp>

#include <dockyard/buffer.hpp>
#include <dockyard/components.hpp>
#include <dockyard/context.hpp>
#include <dockyard/imgui_renderer.hpp>
#include <dockyard/mesh_loader.hpp>
#include <dockyard/scene.hpp>
#include <dockyard/scene_renderer.hpp>
#include <dockyard/vfs.hpp>

#include <GLFW/glfw3.h>
#include <glm/gtc/random.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <imgui.h>

#include <ImGuizmo.h>
#include <vulkan/vulkan_core.h>

#include "./cube_vertices.inl"

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

auto make_default_override_materials(u32) -> std::vector<GPUMaterial>;
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
  app.renderer->bindless.mark_dirty();
}

constexpr auto remove_rotation = [](const auto &m) {
  glm::mat4 result(1.F);

  result[0][0] = glm::length(glm::vec3(m[0]));
  result[1][1] = glm::length(glm::vec3(m[1]));
  result[2][2] = glm::length(glm::vec3(m[2]));

  result[3] = m[3];

  return result;
};

auto make_default_override_materials(u32 count) -> std::vector<GPUMaterial> {
  std::vector<GPUMaterial> output(count);
  for (auto &material : output) {
    material.albedo_factor[0] = material.albedo_factor[1] =
        material.albedo_factor[2] = material.albedo_factor[3] = 1.F;
    material.roughness_factor = 1.F;
    material.normal_scale = 1.F;
    material.occlusion_strength = 1.F;
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
  active_scene = editor_scene.get();
  active_scene->group<Components::Transform, Components::LocalToWorld,
                      Components::Mesh>();
  active_scene->on_construct<Components::Tag>()
      .connect<&Dockforge::on_changed_tag>(*this);
  active_scene->on_destroy<Components::Tag>()
      .connect<&Dockforge::on_changed_tag>(*this);

  auto &&[w, h] = ctx.window_extent;

  editor_camera = std::make_unique<EditorCamera>(
      get_window(), glm::vec3{0.F, 5.F, -6.F}, glm::vec3{0.F, 0.F, 0.F}, w, h);

  {
    imgui_renderer = std::make_unique<ImGuiRenderer>(
        get_window(), 16, *renderer,
        FontChoice{
            .font_path = VFSPath::create("fonts://RobotoMono-Regular.ttf"),
            .size = 15.F,
        });
    imgui_renderer->set_app_name("Dockforge");

    canvas_renderer = std::make_unique<CanvasRenderer>(*renderer);

    viewport_resources = ViewportResources::create(*context, *renderer, w, h);
    renderer->update_output_texture(viewport_resources.forward_target);
    renderer->initialise_bindless();

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
    const int grid_side = 10;
    const float spacing = 0.5F;
    const float offset = (grid_side - 1) * spacing / 2.F;

    auto parent = scene.make("Cubes");

    for (int x = 0; x < grid_side; ++x) {
      for (int y = 0; y < grid_side; ++y) {
        for (int z = 0; z < grid_side; ++z) {
          auto name = std::format("Cube_{}_{}_{}", x, y, z);
          auto entity = scene.make(name);
          entity.get<Components::Transform>().mut().position = {
              (static_cast<float>(x) * spacing) - offset,
              (static_cast<float>(y) * spacing) - offset,
              (static_cast<float>(z) * spacing) - offset,
          };
          entity.emplace<Components::Mesh>(mesh_handle);
          entity.emplace<Components::ParentOf>(parent.handle());
        }
      }
    }
    auto floor = scene.make("Floor");
    floor.emplace<Components::Mesh>(mesh_handle);
    floor.get<Components::Transform>().mut().scale = {30, 1, 30};
    floor.get<Components::Transform>().mut().position = {0, -10, 0};
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
      renderer->textures, renderer->samplers, renderer->comparison_samplers,
      renderer->subimages);

  auto loaded = mesh::load_from_path(
      VFSPath::create("meshes://DamagedHelmet.glb"), *renderer);
  if (loaded) {
    auto entity = active_scene->make("Helmet");
    entity.emplace<Components::Mesh>(*loaded);
    entity.get<Components::Transform>().mut().position = {-5, 3, 9};
  }

  if (auto loaded_sponza = mesh::load_from_path(
          VFSPath::create("meshes://main_sponza.glb"), *renderer)) {
    auto sponza = active_scene->make("Sponza");
    sponza.emplace<Components::Mesh>(*loaded_sponza);
    sponza.get<Components::Transform>().mut().position = {-10, 3, 9};
  }

  auto frustum_entity = active_scene->make("DebugFrustum");
  auto &df_transform = frustum_entity.get<Components::Transform>();
  df_transform.mut().position = glm::vec3{5, -5, -10};
  auto &df = frustum_entity.emplace<Components::DebugFrustum>();
  df.center = glm::vec3{0, 0, 0};
  df.projection_config = {
      .fov_degrees = 30.0F,
      .aspect = 1.77F,
      .near = 0.1F,
      .far = 30.0F,
  };
  df.color = glm::vec4{1.F, 1.F, 0.F, 1.F};
}

auto Dockforge::on_mouse_moved(const events::MouseMoved &e) -> void {
  if (glfwGetMouseButton(get_window(), GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS)
    editor_camera->on_mouse_delta(e.dx, e.dy);
}
auto Dockforge::on_key_released(const events::KeyReleased &e) -> void {
  if (e.key == GLFW_KEY_ESCAPE)
    glfwSetWindowShouldClose(get_window(), GLFW_TRUE);

  if (e.key == GLFW_KEY_F2 && e.mods == GLFW_MOD_SHIFT)
    editor_camera->save_keyframe(2.F);

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

  // 1. View LocalToWorld instead of Transform to get absolute world space
  // positions
  for (auto &&[e, ltw, m] :
       active_scene->view<Components::LocalToWorld, Components::Mesh>()
           .each()) {
    const MeshAsset *asset = renderer->get_mesh(m);
    if ((asset == nullptr) || !asset->mesh_aabb.is_valid())
      continue;

    const auto world_aabb = asset->mesh_aabb.transform(ltw.matrix);
    const auto t = ray_aabb(ray, world_aabb.get_min(), world_aabb.get_max());

    if (t >= 0.0F && t < best_t) {
      best_t = t;
      best = e;
    }
  }

  state.selected = best;
}

void Dockforge::refresh_entity_cache() {
  state.entity_cache.clear();
  auto &registry = active_scene->registry();

  std::vector<entt::entity> roots;
  auto tag_view = registry.view<Components::Tag>();
  for (auto entity : tag_view) {
    if (!registry.any_of<Components::ParentOf>(entity)) {
      roots.push_back(entity);
    }
  }

  auto &cache = state.entity_cache;
  auto child_view = registry.view<Components::ParentOf>();

  auto const add_to_cache_recursive =
      [&, &s = state](this auto &&self, entt::entity current,
                      u32 current_depth) -> void {
    bool has_children = false;
    for (auto child : child_view) {
      if (child_view.get<Components::ParentOf>(child).parent == current) {
        has_children = true;
        break;
      }
    }

    cache.push_back({
        .entity = current,
        .depth = current_depth,
        .is_visible = true,
    });

    if (has_children && s.expanded_entities.contains(current)) {
      for (auto child : child_view) {
        if (child_view.get<Components::ParentOf>(child).parent == current) {
          self(child, current_depth + 1);
        }
      }
    }
  };

  for (auto root : roots) {
    add_to_cache_recursive(root, 0);
  }

  state.cache_dirty = false;
}

void Dockforge::draw_component_editors() {
  if (ImGui::Begin("Component Editor")) {
    if (state.selected != entt::null) {
      Entity entity{*active_scene, state.selected};

      if (auto *tag = entity.try_get<Components::Tag>()) {
        char buffer[128];
        std::snprintf(buffer, sizeof(buffer), "%.*s",
                      static_cast<int>(tag->tag.size()), tag->tag.data());
        if (ImGui::InputText("Tag", buffer, sizeof(buffer))) {
          tag->tag = buffer;
        }
      }

      if (auto *mesh = entity.try_get<Components::Mesh>()) {
        ImGui::Text("Mesh: %u", mesh->handle.index());
      }

      // Debug frustum
      if (auto *frustum = entity.try_get<Components::DebugFrustum>()) {
        ImGui::Text("Debug Frustum:");
        ImGui::SliderFloat3("Center", glm::value_ptr(frustum->center), -20.F,
                            20.F);
        ImGui::SliderFloat("FOV", &frustum->projection_config.fov_degrees, 1.F,
                           179.F);
        ImGui::SliderFloat("Aspect", &frustum->projection_config.aspect, 0.1F,
                           10.F);
        ImGui::SliderFloat("Near", &frustum->projection_config.near, 0.01F,
                           100.F);
        ImGui::SliderFloat("Far", &frustum->projection_config.far, 0.1F,
                           1000.F);
        ImGui::ColorEdit4("Color", glm::value_ptr(frustum->color));
      }
    } else {
      ImGui::Text("No entity selected");
    }
    ImGui::End();
  }
}

void Dockforge::draw_scene_outliner() {
  if (state.cache_dirty) {
    refresh_entity_cache();
  }

  if (!ImGui::Begin("Entities")) {
    ImGui::End();
    return;
  }

  static ImGuiTextFilter filter;
  filter.Draw("##filter", -1.0F);

  ImGui::Separator();

  const float item_height = ImGui::GetTextLineHeightWithSpacing();
  ImGui::BeginChild("##entity_list", ImVec2(0, 0), ImGuiChildFlags_None,
                    ImGuiWindowFlags_HorizontalScrollbar);

  ImGuiListClipper clipper;
  clipper.Begin(static_cast<int>(state.entity_cache.size()), item_height);

  auto &registry = active_scene->registry();
  auto child_view = registry.view<Components::ParentOf>();

  while (clipper.Step()) {
    for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
      const auto &cached = state.entity_cache[static_cast<usize>(i)];
      Entity entity{*active_scene, cached.entity};

      auto *tag = entity.try_get<Components::Tag>();
      auto *mesh = entity.try_get<Components::Mesh>();

      if (tag == nullptr)
        continue;

      const std::string_view label = tag->tag;
      if (!filter.PassFilter(label.data(), std::cend(label)))
        continue;

      // 1. Handle Indentation
      if (cached.depth > 0) {
        ImGui::Indent(static_cast<float>(cached.depth) * 16.0f);
      }

      // 2. Check if this entity has children to determine if we show a dropdown
      // arrow
      bool has_children = false;
      for (auto child : child_view) {
        if (child_view.get<Components::ParentOf>(child).parent ==
            cached.entity) {
          has_children = true;
          break;
        }
      }

      bool is_expanded = state.expanded_entities.contains(cached.entity);

      // 3. Draw Dropdown Arrow
      ImGui::PushID(static_cast<int>(static_cast<uint32_t>(cached.entity)));
      if (has_children) {
        ImGuiDir arrow_dir = is_expanded ? ImGuiDir_Down : ImGuiDir_Right;
        if (ImGui::ArrowButton("##toggle", arrow_dir)) {
          if (is_expanded) {
            state.expanded_entities.erase(cached.entity);
          } else {
            state.expanded_entities.insert(cached.entity);
          }
          state.cache_dirty =
              true; // Rebuild cache on next frame since size changed!
        }
        ImGui::SameLine();
      } else {
        // Keep spacing aligned for leaf items without arrows
        ImGui::Dummy(ImVec2(ImGui::GetFrameHeight(), 0));
        ImGui::SameLine();
      }

      // 4. Draw Row Text Content
      std::array<char, 128> row_label{};
      if (mesh != nullptr) {
        std::snprintf(row_label.data(), std::size(row_label), "[M] %.*s  (%u)",
                      static_cast<int>(label.size()), label.data(),
                      mesh->handle.index());
      } else {
        std::snprintf(row_label.data(), std::size(row_label), "%.*s",
                      static_cast<int>(label.size()), label.data());
      }

      const bool is_selected = (state.selected == cached.entity);
      if (ImGui::Selectable(row_label.data(), is_selected,
                            ImGuiSelectableFlags_SpanAllColumns)) {
        state.selected = cached.entity;
      }

      ImGui::PopID(); // entity ID

      if (cached.depth > 0) {
        ImGui::Unindent(static_cast<float>(cached.depth) * 16.0f);
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

/**
struct GPUMaterial {
  alignas(16) float albedo_factor[4];
  alignas(16) float emissive_factor[4];

  // PBR factors + scales
  float metallic_factor;
  float roughness_factor;
  float normal_scale;
  float occlusion_strength;

  // Alpha & mode
  u32 alpha_mode;
  float alpha_cutoff;

  // Texture indices
  u32 albedo_index;
  u32 normal_index;
  u32 metallic_roughness_index;
  u32 emissive_index;
  u32 occlusion_index;

  // Flags for shader branching
  MaterialFlags flags;

  // ───────────────────────────────────────────────────────────────────────
  // Extensions & advanced features
  // ───────────────────────────────────────────────────────────────────────

  // Transmission (glass refraction) — KHR_materials_transmission
  float transmission_factor; // [0,1]: 0 = opaque, 1 = fully transmissive

  // Anisotropy (brushed metals, etc) — KHR_materials_anisotropy
  float anisotropy_factor;   // [0,1]: strength of anisotropic reflection
  float anisotropy_rotation; // [0,1]: rotation angle (normalized to [0, 2π])

  // Cull mode: determines which faces to render
  u32 cull_mode;

  // UV transformation (cheap variation without extra textures)
  float uv_scale_x;
  float uv_scale_y;
  float uv_offset_x;
  float uv_offset_y;
}; */
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
  changed |=
      ImGui::SliderFloat("Transmission", &mat.transmission_factor, 0.0F, 1.0F);
  changed |=
      ImGui::SliderFloat("Anisotropy", &mat.anisotropy_factor, 0.0F, 1.0F);
  changed |= ImGui::SliderFloat("Anisotropy Rotation", &mat.anisotropy_rotation,
                                0.0F, 1.0F);

  static constexpr std::array<const char *, 3> alpha_modes = {
      "Opaque",
      "Mask",
      "Blend",
  };
  int alpha_mode = static_cast<int>(mat.alpha_mode);
  if (ImGui::Combo("Alpha Mode", &alpha_mode, alpha_modes.data(),
                   std::size(alpha_modes))) {
    mat.alpha_mode = static_cast<u32>(alpha_mode);
    changed = true;
  }

  ImGui::SeparatorText("Texture Slots");
  ImGui::BeginDisabled();

  auto texture_row = [](const char *label, u32 index) {
    static constexpr u32 k_none = ~0U;
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

    ImGui::SameLine(0.0F, ImGui::GetStyle().ItemInnerSpacing.x);
    // vertically centre the InputInt against the icon
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() +
                         ((icon_size - ImGui::GetFrameHeight()) * 0.5F));
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

  for (auto &&[e, frustum, xt] :
       active_scene->view<Components::DebugFrustum, Components::Transform>()
           .each()) {
    auto &&[view, proj] = frustum.matrices(xt.get().position);
    canvas_renderer->frustum(view, proj, frustum.color);
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

  MutableMaterialView selected_entity_materials{};
  if (state.selected != entt::null) {
    Entity selected_entity{*active_scene, state.selected};
    auto &transform = selected_entity.get<Components::Transform>();
    auto &ltw = selected_entity.get<Components::LocalToWorld>();
    auto &&[view, proj] = resolve_camera();

    glm::mat4 world_matrix = ltw.matrix;

    ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj), gizmo_op,
                         ImGuizmo::LOCAL, glm::value_ptr(world_matrix));

    if (ImGuizmo::IsUsing()) {
      glm::mat4 local_matrix = world_matrix;

      if (auto *relation = selected_entity.try_get<Components::ParentOf>();
          relation) {
        auto &registry = active_scene->registry();
        if (registry.valid(relation->parent)) {
          if (auto *parent_ltw =
                  registry.try_get<Components::LocalToWorld>(relation->parent);
              parent_ltw) {
            local_matrix = glm::inverse(parent_ltw->matrix) * world_matrix;
          }
        }
      }

      glm::vec3 new_pos;
      glm::vec3 new_scale;
      glm::vec3 skew;
      glm::vec4 persp;
      glm::quat new_rot;

      if (glm::decompose(local_matrix, new_scale, new_rot, new_pos, skew,
                         persp)) {
        auto &&[pos, rot, scale] = transform.mut();
        pos = new_pos;
        rot = glm::normalize(new_rot);
        scale = new_scale;

        state.hierarchy_dirty = true;
      }
    }

    if (auto *mesh = selected_entity.try_get<Components::Mesh>()) {
      auto *resolved = renderer->get_mesh(*mesh);
      canvas_renderer->box(remove_rotation(world_matrix), resolved->mesh_aabb,
                           glm::vec4{0.1, 0.9, 0.2, 1.0F});

      selected_entity_materials = renderer->get_material_view_mut(*mesh);
    }
  }
  ImGui::End();

  if (ImGui::Begin("Materials")) {
    Entity selected_entity{*active_scene, state.selected};
    if (!selected_entity.valid()) {
      ImGui::TextDisabled("No entity selected");
      ImGui::End();
    } else {
      auto *mesh = selected_entity.try_get<Components::Mesh>();

      if (auto *material_override =
              selected_entity.try_get<Components::MaterialOverride>()) {
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
          remove_override(selected_entity);
          was_removed = true;
        }
        ImGui::PopStyleColor(3);

        if (!was_removed) {
          ImGui::Separator();
          ImGui::PushID("override");
          if (draw_material_editor(material_override->material))
            material_override->dirty = true;
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
            auto &new_ov =
                selected_entity.emplace<Components::MaterialOverride>();
            new_ov.material = mats.first();
            auto random_color =
                glm::linearRand(glm::vec4{0.5F, 0.5F, 0.5F, 1.0F},
                                glm::vec4{1.0F, 1.0F, 1.0F, 1.0F});
            std::ranges::copy(glm::value_ptr(random_color),
                              glm::value_ptr(random_color) + 4,
                              new_ov.material.albedo_factor);
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

  draw_debug_shapes();
  canvas_renderer->render_2d();

  if (ImGui::Begin("Sun direction")) {
    glm::vec4 copy = renderer->sun_direction;
    if (ImGui::DragFloat4("Sun direction", glm::value_ptr(copy), 0.1F)) {
      renderer->sun_direction =
          glm::vec4(glm::normalize(glm::vec3(copy)), 0.0F);
    }
    ImGui::End();
  }

  draw_scene_outliner();

  draw_component_editors();

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
  renderer.reset();
}

auto update_local_to_world_matrices(entt::registry &registry) -> void {
  auto render_group =
      registry.group<Components::Transform, Components::LocalToWorld,
                     Components::Mesh>();

  for (auto e : render_group) {
    auto &transform = render_group.get<Components::Transform>(e);

    if (!transform.dirty())
      continue;

    auto &ltw = render_group.get<Components::LocalToWorld>(e);

    auto &&[position, rotation, scale] = transform.get();

    // Compute the matrix
    ltw.matrix = glm::translate(glm::mat4{1.0F}, position) *
                 glm::mat4_cast(rotation) * glm::scale(glm::mat4{1.0F}, scale);

    // Reset the flag
    transform.set_dirty(false);
  }
}

auto Dockforge::update(float ts) -> void {
  if (active_scene->primary_camera() == nullptr)
    editor_camera->update(ts);

  update_local_to_world_matrices(active_scene->registry());
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

void compute_world_matrices(entt::registry &registry) {
  registry.sort<Components::ParentOf>(
      [&registry](const entt::entity lhs, const entt::entity rhs) {
        auto *lhs_parent = registry.try_get<Components::ParentOf>(lhs);
        auto *rhs_parent = registry.try_get<Components::ParentOf>(rhs);

        if (rhs_parent && rhs_parent->parent == lhs)
          return true;
        if (lhs_parent && lhs_parent->parent == rhs)
          return false;

        return lhs < rhs;
      });

  auto base_view =
      registry.view<Components::Transform, Components::LocalToWorld>();
  for (auto &&[entity, xt, ltw] : base_view.each()) {
    ltw.matrix = xt.matrix(); // Or xt.to_mat4() based on your implementation
  }

  auto hierarchy_view =
      registry.view<Components::ParentOf, Components::LocalToWorld>();

  hierarchy_view.use<Components::ParentOf>();

  for (auto &&[entity, relation, ltw] : hierarchy_view.each()) {
    if (registry.valid(relation.parent)) {
      if (auto *parent_ltw =
              registry.try_get<Components::LocalToWorld>(relation.parent);
          parent_ltw) {
        ltw.matrix = parent_ltw->matrix * ltw.matrix;
      }
    }
  }
}

auto Dockforge::render(RenderContext &ctx) -> u64 {
  if (state.hierarchy_dirty) [[unlikely]] {
    compute_world_matrices(active_scene->registry());
    state.hierarchy_dirty = false;
  }

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

  auto render_group =
      active_scene->group<Components::Transform, Components::LocalToWorld,
                          Components::Mesh>();

  for (auto &&[e, xt, ltw, m] : render_group.each()) {
    renderer->submit(m.handle, ltw.matrix, forward_pipeline.index(),
                     resolve_material_slot({*active_scene, e}));
  }
  auto [view, projection] = resolve_camera();
  renderer->update_csm(view, projection, editor_camera->near_plane(),
                       editor_camera->far_plane());
  if (!renderer->prepare(ctx.frame_index, view, projection)) {
    return ctx.next_frame_wait_value();
  }

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
    renderer->culling_pass(ctx.main_cb);
  }

  {
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

  // ── 2. Depth pre-pass ───────────────────────────────────────────────
  {
    VkRenderingAttachmentInfo depth_attachment{};
    depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth_attachment.imageView = viewport_resources.depth_msaa.sampled_view;
    depth_attachment.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
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
    vkCmdSetDepthBias(ctx.main_cb, 1.25f, 0.F, 1.75f);
    vkCmdBindIndexBuffer(ctx.main_cb,
                         renderer->geometry_pool->index_buffer->get_buffer(),
                         0U, VK_INDEX_TYPE_UINT32);
    renderer->render_pass(ctx.main_cb, renderer->depth_prepass,
                          renderer->pipeline_registry->get(depth_pipeline));
    vkCmdEndRendering(ctx.main_cb);
  }

  // ── 4. Forward MSAA pass ────────────────────────────────────────────
  {
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
    vkCmdSetDepthCompareOp(ctx.main_cb, VK_COMPARE_OP_GREATER_OR_EQUAL);
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
  }

  {
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
  }

  {
    const std::array<VkImageMemoryBarrier2, 1> swapchain_barriers{
        VkImageMemoryBarrier2{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .pNext = nullptr,
            .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
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

  // ── 7. Present barrier ──────────────────────────────────────────────
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

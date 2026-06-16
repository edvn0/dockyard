#include "dockyard/bindless_handle.hpp"
#include <algorithm>
#include <dockforge/dockforge.hpp>
#include <dockforge/inspector_panel.hpp>
#include <dockforge/scene_outliner_panel.hpp>
#include <fstream>
#include <unordered_map>

#include <dockforge/editor_camera.hpp>
#include <dockforge/editor_utils.hpp>
#include <dockforge/matrix_cache.hpp>

#include <dockyard/asset_loader.hpp>
#include <dockyard/binary_stream.hpp>
#include <dockyard/buffer.hpp>
#include <dockyard/components.hpp>
#include <dockyard/context.hpp>
#include <dockyard/game_dll.hpp>
#include <dockyard/game_memory.hpp>
#include <dockyard/imgui_renderer.hpp>
#include <dockyard/mesh_loader.hpp>
#include <dockyard/scene.hpp>
#include <dockyard/scene_renderer.hpp>
#include <dockyard/scene_serialiser.hpp>
#include <dockyard/vfs.hpp>

#include <GLFW/glfw3.h>
#include <dockforge/component_inspector.hpp>
#include <dockforge/component_renderers.hpp>
#include <glm/gtc/random.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <imgui.h>

#include <ImGuizmo.h>

#include <implot.h>

#include <tracy/Tracy.hpp>

#include <dockyard/image_decoder.hpp>
#include <nfd.hpp>

#ifdef _WIN32
#include <windowsx.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

static constexpr float k_step_dt = 1.0F / 60.0F;
static constexpr ImVec2 k_icon_size{20.0F, 20.0F};
static constexpr float k_titlebar_height = 32.0F;
static constexpr float k_titlebar_btn_w = 46.0F;
static constexpr int k_titlebar_btn_count = 3;

#ifdef _WIN32
static constexpr LONG k_titlebar_height_px = 32;
static constexpr LONG k_titlebar_btns_px =
    static_cast<LONG>(k_titlebar_btn_w * k_titlebar_btn_count);

static WNDPROC g_original_wndproc =
    nullptr; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

static LRESULT CALLBACK dockforge_wndproc(HWND hwnd, UINT msg, WPARAM wparam,
                                          LPARAM lparam) {
  if (msg == WM_NCHITTEST) {
    const LRESULT hit =
        CallWindowProcW(g_original_wndproc, hwnd, msg, wparam, lparam);
    if (hit == HTCLIENT) {
      POINT cursor{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      ScreenToClient(hwnd, &cursor);
      if (cursor.y >= 0 && cursor.y < k_titlebar_height_px) {
        RECT rect{};
        GetClientRect(hwnd, &rect);
        // Right-side button area stays HTCLIENT so ImGui receives clicks.
        if (cursor.x < rect.right - k_titlebar_btns_px)
          return HTCAPTION;
      }
    }
    return hit;
  }
  if (msg == WM_GETMINMAXINFO) {
    // Prevent a borderless-maximised window from covering the taskbar.
    auto *mmi = reinterpret_cast<LPMINMAXINFO>(lparam);
    const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(MONITORINFO);
    GetMonitorInfoW(monitor, &mi);
    mmi->ptMaxPosition = {mi.rcWork.left, mi.rcWork.top};
    mmi->ptMaxSize = {mi.rcWork.right - mi.rcWork.left,
                      mi.rcWork.bottom - mi.rcWork.top};
    return 0;
  }
  return CallWindowProcW(g_original_wndproc, hwnd, msg, wparam, lparam);
}

static auto install_titlebar_hit_test(GLFWwindow *window) -> void {
  HWND hwnd = glfwGetWin32Window(window);
  g_original_wndproc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
      hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(dockforge_wndproc)));
}
#endif

namespace {

struct AssetLoader : dy::IAssetLoader {
  dy::SceneRenderer &renderer;
  StringMap<dy::MeshAssetHandle> mesh_cache;

  explicit AssetLoader(dy::SceneRenderer &r) : renderer(r) {}

  auto load_mesh(const dy::VFSPath &path)
      -> std::expected<dy::MeshAssetHandle, std::string> override {
    if (auto it = mesh_cache.find(path.view()); it != mesh_cache.end())
      return it->second;
    auto result = dy::mesh::load_from_path(path, renderer);
    if (result)
      mesh_cache.emplace(std::string{path.view()}, *result);
    return result;
  }
};

inline auto pack_normal(glm::vec3 n) {
  return glm::packSnorm4x8(glm::vec4(n, 0.0F));
}
inline auto pack_uv(glm::vec2 uv) { return glm::packHalf2x16(uv); }

#include "./capsule_vertices.inl"
#include "./cube_vertices.inl"

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
} // namespace

auto make_app() -> std::unique_ptr<Dockforge> {
  return std::make_unique<Dockforge>();
}

Dockforge::~Dockforge() = default;

static constexpr std::string_view k_gamedll_scheme = "gamedll";
static const std::filesystem::path k_dll_settings_path =
    std::filesystem::path(ASSETS_ROOT_PATH) / "editor" / "game_dll_path.txt";

auto Dockforge::init(const InitialisationContext &ctx) -> void {
  NFD::Init();

  renderer =
      std::make_unique<SceneRenderer>(ctx.context, ctx.swapchain_resources);
  asset_loader = std::make_unique<AssetLoader>(*renderer);
  context = &ctx.context;

  game_memory = GameMemory::create();

  // Try the last-used DLL path; fall back to the built-in sandbox.
  std::filesystem::path dll_to_load;
  if (std::ifstream ifs{k_dll_settings_path}; ifs) {
    std::string line;
    if (std::getline(ifs, line) && !line.empty())
      dll_to_load = line;
  }
  if (dll_to_load.empty() || !std::filesystem::exists(dll_to_load)) {
    dll_to_load = std::filesystem::path(ASSETS_ROOT_PATH) / "binary" /
                  std::format("sandbox.{}", shared_extension);
  }
  load_game_dll(dll_to_load);

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
  }

  {
    cube_mesh_handle =
        mesh::load_from_memory(*renderer, cube_verts, cube_indices).value();
    auto capsule_mesh_handle =
        mesh::load_from_memory(*renderer, capsule_verts, capsule_indices)
            .value();

    auto &scene = *active_scene;
    constexpr int grid_side = 10;

    auto cube = scene.make("Cube");
    cube.emplace<Components::Mesh>(cube_mesh_handle);
    cube.get<Components::Transform>().mut().scale = {
        grid_side,
        grid_side,
        grid_side,
    };

    auto human_like = scene.make("Human");
    auto xt = human_like.get<Components::Transform>().mut();
    xt.position = glm::vec3(0.0f, 0.875f, 0.0f);
    human_like.emplace<Components::Mesh>(capsule_mesh_handle);
    xt.scale = glm::vec3(0.2f, 1.75f, 0.2f);

    auto floor = scene.make("Floor");
    floor.emplace<Components::Mesh>(cube_mesh_handle);
    floor.get<Components::Transform>().mut().scale = {30, 1, 30};
    floor.get<Components::Transform>().mut().position = {0, -10, 0};

    auto light_parent = scene.make("Light parent");
    const auto floor_pos = glm::vec3{0, -10, 0};
    for (int i = 0; i < 128; i++) {
      auto light = scene.make("Light", light_parent);
      auto &l = light.emplace<Components::PointLight>();
      l.color = glm::linearRand(glm::vec3{0.5F}, glm::vec3{1.F});
      l.intensity = glm::linearRand(0.5F, 5.F);
      l.radius = glm::linearRand(5.F, 20.F);
      auto &t = light.get<Components::Transform>();
      t.mut().position =
          floor_pos + glm::vec3{glm::linearRand(-15.F, 15.F), 2.F,
                                glm::linearRand(-15.F, 15.F)};
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
      renderer->textures, renderer->samplers, renderer->comparison_samplers,
      renderer->subimages);

  if (auto loaded_sponza = mesh::load_from_path(
          VFSPath::create("meshes://Sponza/MISSING_main_sponza.glb"),
          *renderer)) {
    auto sponza = active_scene->make("Sponza");
    sponza.emplace<Components::Mesh>(*loaded_sponza);
    sponza.get<Components::Transform>().mut().position = {-10, 3, 9};
  }

  editor_state.active_scene = active_scene;
  editor_state.renderer = renderer.get();

  editor_actions.duplicate_entity = [this](Entity ent) {
    return duplicate_entity(ent);
  };
  editor_actions.create_entity = [this](std::string_view name) {
    editor_state.cache_dirty = true;
    return active_scene->make(name);
  };
  editor_actions.destroy_entity = [this](Entity ent) {
    active_scene->destroy_and_all_children(ent.handle(), *renderer);
    editor_state.cache_dirty = true;
  };

  panels.push_back(std::make_unique<SceneOutlinerPanel>());
  panels.push_back(std::make_unique<InspectorPanel>());

  load_toolbar_icons();

  auto frustum_entity = active_scene->make("DebugFrustum");
  auto &df_transform = frustum_entity.get<Components::Transform>();
  df_transform.mut().position = glm::vec3{5, -5, -10};
  auto &df = frustum_entity.emplace<Components::DebugFrustum>();
  df.projection_config = {
      .fov_degrees = 30.0F,
      .aspect = 1.77F,
      .near_plane = 0.1F,
      .far_plane = 30.0F,
  };
  df.color = glm::vec4{1.F, 1.F, 0.F, 1.F};

#ifdef _WIN32
  install_titlebar_hit_test(get_window());
#endif
}

auto Dockforge::configure_window_hints() -> void {
  glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
}

auto Dockforge::draw_titlebar() -> void {
  auto window = App::get_window();
  const ImGuiViewport *vp = ImGui::GetMainViewport();

  ImGui::SetNextWindowPos(vp->Pos);
  ImGui::SetNextWindowSize({vp->Size.x, k_titlebar_height});
  ImGui::SetNextWindowViewport(vp->ID);

  constexpr ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
      ImGuiWindowFlags_NoSavedSettings |
      ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoDocking;

  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0F);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0F, 0.0F});
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4{0.08F, 0.08F, 0.09F, 1.0F});
  ImGui::Begin("##Titlebar", nullptr, flags);
  ImGui::PopStyleVar(3);
  ImGui::PopStyleColor();

  const float right = ImGui::GetContentRegionMax().x;
  const float center_y =
      (k_titlebar_height - ImGui::GetTextLineHeight()) * 0.5F;
  const float drag_w =
      right - k_titlebar_btn_w * static_cast<float>(k_titlebar_btn_count);

  // ── Invisible drag region (input layer) ───────────────────────────────────
  // On Win32 the HTCAPTION wndproc intercepts these events at the OS level,
  // so this code only fires on X11 / Wayland.
  ImGui::SetCursorPos({0.0F, 0.0F});
  ImGui::InvisibleButton("##drag", {drag_w, k_titlebar_height});

  if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
    const ImVec2 delta = ImGui::GetIO().MouseDelta;
    int win_x = 0;
    int win_y = 0;
    glfwGetWindowPos(window, &win_x, &win_y);
    glfwSetWindowPos(window, win_x + static_cast<int>(delta.x),
                     win_y + static_cast<int>(delta.y));
  }

  if (ImGui::IsItemHovered() &&
      ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
    const bool maximised = glfwGetWindowAttrib(window, GLFW_MAXIMIZED) != 0;
    maximised ? glfwRestoreWindow(window) : glfwMaximizeWindow(window);
  }

  // ── Visual content drawn over the drag region ─────────────────────────────
  ImGui::SetCursorPos({8.0F, center_y});
  ImGui::TextColored({0.55F, 0.55F, 0.60F, 1.0F}, "Dockforge");

  ImGui::SameLine();
  ImGui::SetCursorPosY(center_y);
  ImGui::TextDisabled("|");
  ImGui::SameLine();
  ImGui::SetCursorPosY(center_y);
  if (sim_state.in<sim::S::Playing>())
    ImGui::TextColored({0.35F, 0.85F, 0.35F, 1.0F}, "Playing");
  else if (sim_state.in<sim::S::Paused>())
    ImGui::TextColored({0.90F, 0.70F, 0.25F, 1.0F}, "Paused");
  else
    ImGui::TextDisabled("Editor");

  // ── Window control buttons ────────────────────────────────────────────────
  constexpr float btn_w = k_titlebar_btn_w;
  constexpr float btn_h = k_titlebar_height;

  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0F);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {0.0F, 0.0F});
  ImGui::PushStyleColor(ImGuiCol_Button, {0.0F, 0.0F, 0.0F, 0.0F});
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {1.0F, 1.0F, 1.0F, 0.08F});
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, {1.0F, 1.0F, 1.0F, 0.16F});

  ImGui::SetCursorPos({right - btn_w * 3.0F, 0.0F});
  if (ImGui::Button("_##min", {btn_w, btn_h}))
    glfwIconifyWindow(window);

  ImGui::SetCursorPos({right - btn_w * 2.0F, 0.0F});
  const bool maximised = glfwGetWindowAttrib(window, GLFW_MAXIMIZED) != 0;
  if (ImGui::Button(maximised ? "#r##max" : "#m##max", {btn_w, btn_h}))
    maximised ? glfwRestoreWindow(window) : glfwMaximizeWindow(window);

  ImGui::SetCursorPos({right - btn_w, 0.0F});
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.85F, 0.18F, 0.18F, 1.0F});
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, {0.70F, 0.10F, 0.10F, 1.0F});
  if (ImGui::Button("x##close", {btn_w, btn_h}))
    glfwSetWindowShouldClose(window, GLFW_TRUE);
  ImGui::PopStyleColor(2);

  ImGui::PopStyleColor(3);
  ImGui::PopStyleVar(2);

  ImGui::End();
}

auto Dockforge::on_mouse_moved(const events::MouseMoved &e) -> void {
  if (glfwGetMouseButton(App::get_window(), GLFW_MOUSE_BUTTON_RIGHT) ==
      GLFW_PRESS)
    editor_camera->on_mouse_delta(e.dx, e.dy);
}

auto Dockforge::on_mouse_scrolled(const events::MouseScrolled &e) -> void {
  editor_camera->on_mouse_scrolled(e);
}

auto Dockforge::on_key_released(const events::KeyReleased &e) -> void {
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

  // LocalToWorld gives absolute world-space positions; Transform is local only
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

  editor_state.selected = best;
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

auto Dockforge::duplicate_entity(Entity src) -> Entity {
  static constexpr std::string_view name = "duplicate";
  auto dst = active_scene->make(name);

  for_each_type<MasterComponentList>([&]<typename T>() {
    if (auto *comp = src.try_get<T>())
      dst.emplace_or_replace<T>(*comp);
  });

  if (auto *tag = dst.try_get<Components::Tag>())
    tag->tag += " (copy)";

  if (auto *ov = dst.try_get<Components::MaterialOverride>()) {
    ov->gpu_slot = Components::MaterialOverride::invalid_material;
    ov->dirty = true;
  }

  editor_state.cache_dirty = true;
  return dst;
}

auto Dockforge::draw_debug_shapes() -> void {
  ZoneScopedNC("Dockforge::draw_debug_shapes", 0xAAAAAA);
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
    auto &&[pos, rot, scl] = xt.get();
    auto &&[view, proj] = frustum.matrices(pos, rot);
    canvas_renderer->frustum(view, proj, frustum.color);
  }
}

namespace {
struct SpanTelemetry {
  static constexpr size_t max_points = 500;

  std::array<double, max_points * 2> storage_pool{};
  size_t write_index = 0;

  auto push(double dt_ms) -> void {
    storage_pool[write_index] = dt_ms;
    storage_pool[write_index + max_points] = dt_ms;

    write_index = (write_index + 1) % max_points;
  }

  [[nodiscard]] auto get_history_span() const noexcept
      -> std::span<const double, max_points> {
    return std::span<const double, max_points>(&storage_pool[write_index],
                                               max_points);
  }
};
SpanTelemetry telemetry;
} // namespace

auto draw_performance_overlay() -> void {
  auto current_dt_ms = static_cast<double>(ImGui::GetIO().DeltaTime * 1000.0F);
  telemetry.push(current_dt_ms);

  if (ImGui::Begin("Telemetry Profiler")) {
    ImGui::Text("FPS: %.1f | Current Frame: %.2f ms", ImGui::GetIO().Framerate,
                current_dt_ms);

    ImPlot::SetNextAxesToFit();
    ImPlot::SetNextAxisLimits(ImAxis_Y1, 0.0, 40.0, ImGuiCond_Always);

    if (ImPlot::BeginPlot("##FrametimeGraph", ImVec2(-1, 150),
                          ImPlotFlags_NoLegend)) {

      ImPlot::SetupAxis(ImAxis_X1, "Frames (History)", ImPlotAxisFlags_None);
      ImPlot::SetupAxis(ImAxis_Y1, "Frametime (ms)", ImPlotAxisFlags_None);

      double budget_60fps = 16.66;
      double budget_30fps = 33.33;
      ImPlot::DragLineY(0, &budget_60fps, ImVec4(0.0f, 1.0f, 0.0f, 0.4f), 1.0f,
                        1);
      ImPlot::DragLineY(1, &budget_30fps, ImVec4(1.0f, 0.5f, 0.0f, 0.4f), 1.0f,
                        1);

      auto history_view = telemetry.get_history_span();

      ImPlot::PlotLine("Frametime", history_view.data(),
                       static_cast<int>(history_view.size()), 1.0, 0.0);

      ImPlot::EndPlot();
    }
  }
  ImGui::End();
}

auto Dockforge::draw_hdr_selector() -> void {
  static NullableVFSPath selected_hdr;

  if (!ImGui::Begin("Environment")) {
    ImGui::End();
    return;
  }

  ImGui::Text("HDR map");

  if (!selected_hdr.valid()) {
    ImGui::TextDisabled("No external HDR selected");
  } else {
    ImGui::TextWrapped("%.*s", static_cast<int>(selected_hdr.view().size()),
                       selected_hdr.view().data());
  }

  if (ImGui::Button("Browse HDR...")) {
    NFD::Guard nfd_guard;

    const nfdfilteritem_t filters[] = {
        {"HDR images", "hdr,exr"},
        {"All files", "*"},
    };

    NFD::UniquePath out_path;
    const nfdresult_t result =
        NFD::OpenDialog(out_path, filters, std::size(filters));

    if (result == NFD_OKAY) {
      const auto virtual_hdr_path = VFS::get().mount_file(
          "external_hdr", std::filesystem::path{out_path.get()});

      selected_hdr = NullableVFSPath{virtual_hdr_path};
      renderer->set_hdr_map(virtual_hdr_path);
    } else if (result == NFD_ERROR) {
      warn("Native file dialog failed: {}", NFD::GetError());
    }
  }

  ImGui::SameLine();

  if (ImGui::Button("Use packaged sunset")) {
    const auto sunset = VFSPath::create("textures://env/sunset_f16.ktx2");
    selected_hdr = NullableVFSPath{sunset};
    renderer->set_hdr_map(sunset);
  }

  if (renderer->ibl_probe.valid()) {
    ImGui::SeparatorText("GPU handles");
    ImGui::Text("Env map:     %u", renderer->ibl_probe.env_map.index());
    ImGui::Text("Irradiance:  %u", renderer->ibl_probe.irradiance.index());
    ImGui::Text("Prefiltered: %u", renderer->ibl_probe.prefiltered.index());
    ImGui::Text("BRDF LUT:    %u", renderer->ibl_probe.brdf_lut.index());
  }

  ImGui::End();
}
auto Dockforge::load_toolbar_icons() -> void {
  const std::array<std::pair<std::string_view, TextureHandle *>, 5> icons{{
      {"editor://icons/play_32.png", &icon_play},
      {"editor://icons/pause_32.png", &icon_pause},
      {"editor://icons/stop_32.png", &icon_stop},
      {"editor://icons/step_32.png", &icon_step},
      {"editor://icons/reload_32.png", &icon_reload},
  }};

  for (auto &&[path_str, handle] : icons) {
    std::stop_source stop_src;
    auto fut = std::async(
        std::launch::async,
        [path = VFSPath::create(path_str)]() -> dy::pool::CpuTextureData {
          auto decoded = dy::ImageDecoder::from_path(path);
          if (!decoded) {
            warn("Toolbar icon: {}", decoded.error());
            return {};
          }

          dy::pool::CpuTextureData data;
          auto &mip = data.mips.emplace_back(dy::pool::MipData{
              .pixels = std::move(decoded->pixels),
              .width = decoded->width,
              .height = decoded->height,
          });
          data.pixels = mip.pixels;
          data.name = std::string(path.stem());
          data.width = decoded->width;
          data.height = decoded->height;
          data.format = VK_FORMAT_R8G8B8A8_UNORM;
          return data;
        });

    renderer->texture_upload_pool->submit(
        std::move(fut), std::move(stop_src),
        [handle](TextureHandle h) { *handle = h; });
  }
}

auto Dockforge::draw_toolbar() -> void {
  if (!ImGui::Begin("Toolbar")) {
    ImGui::End();
    return;
  }

  const bool editing = sim_state.in<sim::S::Editing>();
  const bool playing = sim_state.in<sim::S::Playing>();
  const bool paused = sim_state.in<sim::S::Paused>();
  const bool has_dll = game_dll && game_dll->game();

  auto icon_button = [](const char *id, TextureHandle handle,
                        bool enabled) -> bool {
    ImGui::BeginDisabled(!enabled || !handle.valid());
    const bool pressed = ImGui::ImageButton(
        id, ImTextureRef{ImTextureID{handle.index()}}, k_icon_size);
    ImGui::EndDisabled();
    return pressed && enabled;
  };

  if (icon_button("##play", icon_play, (editing && has_dll) || paused)) {
    if (paused)
      resume();
    else
      play();
  }
  ImGui::SameLine();

  if (icon_button("##pause", icon_pause, playing))
    pause();
  ImGui::SameLine();

  if (icon_button("##stop", icon_stop, playing || paused))
    stop();
  ImGui::SameLine();

  if (icon_button("##step", icon_step, paused && has_dll))
    step();

  if (editing) {
    ImGui::SameLine();

    const auto dll_label = game_dll_path.empty()
                               ? std::string{"No DLL loaded"}
                               : game_dll_path.filename().string();
    const float button_h =
        k_icon_size.y + ImGui::GetStyle().FramePadding.y * 2.0F;
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() +
                         (button_h - ImGui::GetTextLineHeight()) * 0.5F);
    ImGui::TextDisabled("%s", dll_label.c_str());
    if (!game_dll_path.empty() && ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", game_dll_path.string().c_str());
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() -
                         (button_h - ImGui::GetTextLineHeight()) * 0.5F);

    ImGui::SameLine();
    const ImVec2 browse_sz{0.0F, k_icon_size.y +
                                     ImGui::GetStyle().FramePadding.y * 2.0F};
    if (ImGui::Button("...##browse", browse_sz)) {
      nfdnchar_t *out = nullptr;
      const nfdnfilteritem_t filter{
          .name = "Shared Library",
          .spec = "dll,so,dylib",
      };
      if (NFD::OpenDialog(out, &filter, 1) == NFD_OKAY) {
        const std::filesystem::path chosen{out};
        NFD::FreePath(out);
        load_game_dll(chosen);
        if (std::ofstream ofs{k_dll_settings_path}; ofs)
          ofs << chosen.string();
      }
    }

    ImGui::SameLine();
    if (icon_button("##reload", icon_reload, has_dll)) {
      PROFILE_SCOPE("Reload dll");
      load_game_dll(game_dll_path);
    }
  }

  ImGui::End();
}

auto Dockforge::build_ui() -> void {
  ZoneScopedNC("Dockforge::build_ui", 0xFFD700);
  draw_titlebar();

  const ImGuiViewport *vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos({vp->WorkPos.x, vp->WorkPos.y + k_titlebar_height});
  ImGui::SetNextWindowSize(
      {vp->WorkSize.x, vp->WorkSize.y - k_titlebar_height});
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

  if (editor_state.selected != entt::null) {
    Entity selected_entity{*active_scene, editor_state.selected};
    auto &transform = selected_entity.get<Components::Transform>();
    auto &ltw = selected_entity.get<Components::LocalToWorld>();
    auto &&[view, proj] = resolve_camera();

    glm::mat4 world_matrix = ltw.matrix;

    if (auto *mesh = selected_entity.try_get<Components::Mesh>();
        mesh != nullptr) {
      const auto *asset = renderer->resolve(mesh->handle);
      const auto &aabb = asset->mesh_aabb;

      canvas_renderer->box(transform.matrix_without_rotation(), aabb,
                           glm::vec4{0.9F, 0.1F, 0.1F, 1.0F});
    }

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

        editor_state.hierarchy_dirty = true;
      }
    }
  }
  ImGui::End();

  draw_debug_shapes();
  canvas_renderer->render_2d();

  if (ImGui::Begin("Sun direction")) {
    if (ImGui::DragFloat3("Sun direction",
                          glm::value_ptr(renderer->sun_direction), 0.1F, -1.0F,
                          1.0F)) {
      editor_state.hierarchy_dirty = true;
    }

    ImGui::Separator();
    ImGui::SeparatorText("Shadow Settings");
    ImGui::DragFloat("Shadow Distance", &shadow_map_state.far_plane, 1.0F,
                     10.0F, 1000.0F, "%.0f m");
    if (ImGui::IsItemDeactivatedAfterEdit())
      shadow_map_state.invalid = true;
  }
  ImGui::End();

  {
    ZoneScopedNC("draw_toolbar", 0x888888);
    draw_toolbar();
  }
  for (auto &panel : panels)
    panel->draw(editor_state, editor_actions);
  {
    ZoneScopedNC("draw_performance_overlay", 0x888888);
    draw_performance_overlay();
  }
  {
    ZoneScopedNC("draw_hdr_selector", 0x888888);
    draw_hdr_selector();
  }

  auto draw_csm = [](CsmResources &resources) {
    if (ImGui::Begin("Cascaded Shadow Map Debug")) {
      static int selected_cascade = 0;
      if (selected_cascade >= shadow_map_cascade_count) {
        selected_cascade = 0;
      }

      std::string preview_text = "Cascade " + std::to_string(selected_cascade);

      if (ImGui::BeginCombo("Level", preview_text.c_str())) {
        for (int i = 0; i < shadow_map_cascade_count; i++) {
          const bool is_selected = (selected_cascade == i);
          std::string item_text = "Cascade " + std::to_string(i);

          if (ImGui::Selectable(item_text.c_str(), is_selected)) {
            selected_cascade = i;
          }

          if (is_selected) {
            ImGui::SetItemDefaultFocus();
          }
        }
        ImGui::EndCombo();
      }

      ImGui::Image(ImTextureRef{ImTextureID{
                       resources.layer_handles[selected_cascade].index()}},
                   ImVec2(256, 256));
    }
    ImGui::End();
  };
  draw_csm(renderer->csm);
  if (viewport_hovered || viewport_focused) {
    ImGui::GetIO().WantCaptureMouse = false;
    ImGui::GetIO().WantCaptureKeyboard = false;
  }
}

auto Dockforge::destroy() -> void {
  NFD::Quit();

  if (!sim_state.in<sim::S::Editing>())
    stop();

  if (game_dll) {
    game_dll->stop_watching();
    game_dll.reset();
  }
  game_memory.destroy();

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
  ZoneScopedNC("update_local_to_world_matrices", 0x98FB98);
  auto render_group =
      registry.group<Components::Transform, Components::LocalToWorld,
                     Components::Mesh>();

  for (auto e : render_group) {
    auto &transform = render_group.get<Components::Transform>(e);

    if (!transform.dirty())
      continue;

    auto &ltw = render_group.get<Components::LocalToWorld>(e);

    auto &&[position, rotation, scale] = transform.get();

    ltw.matrix = glm::translate(glm::mat4{1.0F}, position) *
                 glm::mat4_cast(rotation) * glm::scale(glm::mat4{1.0F}, scale);

    transform.set_dirty(false);
  }
}

auto Dockforge::load_game_dll(const std::filesystem::path &path) -> void {
  if (game_dll) {
    game_dll->stop_watching();
    game_dll.reset();
  }
  const auto vfs_path = VFS::get().mount_file(k_gamedll_scheme, path);
  if (auto result = GameDll::load(vfs_path)) {
    game_dll_path = path;
    game_dll = std::move(*result);
    game_dll->start_watching(renderer->thread_pool);
    game_dll->game()->pre_init(*asset_loader);
  } else {
    warn("GameDll: {}", result.error());
  }
}

auto Dockforge::pause() -> void {
  sim_state.try_transition<sim::S::Paused>(
      [&](auto) { TracyMessage("Game paused", 11); });
}

auto Dockforge::resume() -> void {
  sim_state.try_transition<sim::S::Playing>(
      [&](auto) { TracyMessage("Game resumed", 12); });
}

auto Dockforge::step() -> void {
  if (!sim_state.in<sim::S::Paused>() || !game_dll || !game_dll->game())
    return;
  game_dll->game()->update(&game_memory, active_scene, k_step_dt);
}

auto Dockforge::play() -> void {
  if (!game_dll || !game_dll->game())
    return;

  sim_state.try_transition<sim::S::Playing>([&](auto) {
    runtime_scene = std::make_shared<Scene>();

    std::vector<u8> snapshot_buf;
    MemoryWriter writer{snapshot_buf};
    SceneSerializer::serialize_parallel(renderer->thread_pool, *editor_scene,
                                        writer);
    MemoryReader reader{snapshot_buf};
    SceneSerializer::deserialize(*runtime_scene, reader);

    runtime_scene->group<Components::Transform, Components::LocalToWorld,
                         Components::Mesh>();

    active_scene = runtime_scene.get();
    editor_state.active_scene = active_scene;
    game_memory.reset();
    game_dll->game()->init(&game_memory, active_scene, *asset_loader);
    TracyMessage("Game started", 12);
    editor_state.cache_dirty = true;
  });
}

auto Dockforge::stop() -> void {
  sim_state.try_transition<sim::S::Editing>([&](auto) {
    if (game_dll && game_dll->game())
      game_dll->game()->destroy(&game_memory, active_scene);

    game_memory.reset();
    TracyMessage("Game stopped", 12);
    active_scene = editor_scene.get();
    editor_state.active_scene = active_scene;
    runtime_scene.reset();
    editor_state.selected = entt::null;
    editor_state.cache_dirty = true;
    for (auto &panel : panels)
      panel->on_stop();
  });
}

auto Dockforge::update(float ts) -> void {
  ZoneScopedNC("Dockforge::update", 0x00BFFF);
  if (!sim_state.in<sim::S::Editing>() && game_dll && game_dll->game()) {
    if (game_dll->poll_reload()) {
      game_dll->game()->destroy(&game_memory, active_scene);
      game_memory.reset();
      game_dll->game()->pre_init(*asset_loader);
      game_dll->game()->init(&game_memory, active_scene, *asset_loader);
      info("Game DLL hot reloaded");
      TracyMessage("GameDLL hot reloaded", 20);
    }
    if (sim_state.in<sim::S::Playing>())
      game_dll->game()->update(&game_memory, active_scene, ts);
  }
  if (active_scene->primary_camera() == nullptr)
    editor_camera->update(ts);

  update_local_to_world_matrices(active_scene->registry());

  /*  active_scene->registry().view<Components::MeshRequest>().each([&](auto e,
   auto &req) { if (auto loaded = mesh::load_from_path(req.path, *renderer)) {
       req.result = *loaded;
     } else {
       warn("Failed to load mesh from path: {}", req.path.string());
     }
     active_scene->registry().remove<Components::MeshRequest>(e);
   }); */
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
        warn("MaterialOverridePool full — override skipped this frame");
      }
    }
    if (override_slot.dirty) {
      renderer->geometry_pool->get_materials_mut(override_slot.gpu_slot, 1)[0] =
          override_slot.material;
      renderer->geometry_pool->flush_material(override_slot.gpu_slot);
      override_slot.dirty = false;
    }
  }
}

namespace {
auto resolve_material_slot(Entity e) -> u32 {
  constexpr u32 default_material = 0U;
  auto *ov = e.try_get<Components::MaterialOverride>();
  if (ov == nullptr ||
      ov->gpu_slot == Components::MaterialOverride::invalid_material)
    return default_material;
  return ov->gpu_slot;
}
} // namespace

void compute_world_matrices(entt::registry &registry) {
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

  auto render_group =
      active_scene->group<Components::Transform, Components::LocalToWorld,
                          Components::Mesh>();

  flush_material_overrides();

  for (auto &&[e, xt, ltw, m] : render_group.each()) {
    renderer->submit(m.handle, ltw.matrix, forward_pipeline.index(),
                     resolve_material_slot({*active_scene, e}));
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
  TracyPlot("mesh_entities", static_cast<int64_t>(render_group.size()));

  auto prepare_result = renderer->prepare({
      .frame_index = ctx.frame_index,
      .view = view,
      .projection = projection,
      .camera_near_far =
          glm::vec2(editor_camera->near_plane(), editor_camera->far_plane()),
      .shadow_near_far =
          glm::vec2(shadow_map_state.near_plane, shadow_map_state.far_plane),
      .point_lights = std::span(gpu_lights.data(), light_count),
  });
  if (prepare_result.failed()) {
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
            .srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
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

#include <dockforge/dockforge.hpp>

#include <dockforge/editor_camera.hpp>
#include <dockforge/inspector_panel.hpp>
#include <dockforge/renderer_settings_panel.hpp>
#include <dockforge/scene_outliner_panel.hpp>
#include <dockforge/texture_pool_panel.hpp>

#include <dockyard/asset_loader.hpp>
#include <dockyard/components.hpp>
#include <dockyard/image_decoder.hpp>
#include <dockyard/imgui_renderer.hpp>
#include <dockyard/mesh_loader.hpp>
#include <dockyard/physics_world.hpp>
#include <dockyard/scene_renderer.hpp>
#include <dockyard/vfs.hpp>

#ifdef _WIN32
#include <windowsx.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#elif defined(__linux__)
#define GLFW_EXPOSE_NATIVE_WAYLAND
#define GLFW_EXPOSE_NATIVE_X11
#elif defined(__APPLE__)
#define GLFW_EXPOSE_NATIVE_COCOA
#endif
#include <nfd.hpp>
#include <nfd_glfw3.h>

using namespace dy;

#ifdef _WIN32
static constexpr LONG titlebar_height_px = 32;
static constexpr LONG titlebar_btns_px =
    static_cast<LONG>(46.0F * 3); // titlebar_btn_w * titlebar_btn_count

static WNDPROC g_original_wndproc = // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
    nullptr;

static LRESULT CALLBACK dockforge_wndproc(HWND hwnd, UINT msg, WPARAM wparam,
                                          LPARAM lparam) {
  if (msg == WM_NCHITTEST) {
    const LRESULT hit =
        CallWindowProcW(g_original_wndproc, hwnd, msg, wparam, lparam);
    if (hit == HTCLIENT) {
      POINT cursor{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      ScreenToClient(hwnd, &cursor);
      if (cursor.y >= 0 && cursor.y < titlebar_height_px) {
        RECT rect{};
        GetClientRect(hwnd, &rect);
        // Right-side button area stays HTCLIENT so ImGui receives clicks.
        if (cursor.x < rect.right - titlebar_btns_px)
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
  struct CacheEntry {
    dy::MeshAssetHandle handle;
    bool has_collision_geometry = false;
  };

  dy::SceneRenderer &renderer;
  StringMap<CacheEntry> mesh_cache;

  explicit AssetLoader(dy::SceneRenderer &r) : renderer(r) {}

  auto load_mesh(const dy::VFSPath &path, bool retain_collision_geometry)
      -> std::expected<dy::MeshAssetHandle, std::string> override {
    if (auto it = mesh_cache.find(path.view()); it != mesh_cache.end()) {
      if (!retain_collision_geometry || it->second.has_collision_geometry)
        return it->second.handle;
      // Previously loaded without collision geometry; fall through and
      // reload with retention so the physics world has geometry to build a
      // mesh collider from. This registers a second MeshAsset — accepted
      // cost for the (rare) case a render mesh is later reused as collision.
    }

    const dy::mesh::LoadOptions opts{
        .retain_collision_geometry = retain_collision_geometry,
    };
    std::expected<MeshAssetHandle, std::string> result{};
    const auto ext = path.extension();
    const bool is_compressed = ext == ".zip" || ext == ".gz" || ext == ".tgz" ||
                               ext == ".zst" || ext == ".tzst" || ext == ".zstd";
    if (is_compressed) {
      result = dy::mesh::load_from_compressed(path, renderer, opts);
    } else {
      result = dy::mesh::load_from_path(path, renderer, opts);
    }
    if (result)
      mesh_cache[std::string{path.view()}] = CacheEntry{
          .handle = *result,
          .has_collision_geometry = retain_collision_geometry,
      };
    return result;
  }

  auto notify_material_overrides_added() -> void override {
    if (renderer.override_pool.free_slots.empty() &&
        renderer.override_pool.next >= renderer.override_pool.capacity)
      renderer.override_pool.needs_grow = true;
  }

  auto make_animation_state(dy::MeshAssetHandle handle, dy::u32 skel_idx,
                            dy::u32 clip_idx)
      -> std::optional<dy::AnimationState> override {
    const auto *asset = renderer.get_mesh(handle);
    if (!asset)
      return std::nullopt;
    if (skel_idx >= asset->skeletons.size())
      return std::nullopt;
    if (clip_idx >= asset->animations.size())
      return std::nullopt;
    return dy::AnimationState::create(&asset->skeletons[skel_idx],
                                      &asset->animations[clip_idx],
                                      static_cast<dy::i32>(skel_idx));
  }
};

inline auto pack_normal(glm::vec3 n) {
  return glm::packSnorm4x8(glm::vec4(n, 0.0F));
}

inline auto pack_uv(glm::vec2 uv) { return glm::packHalf2x16(uv); }

#include "../capsule_vertices.inl"
#include "../cube_vertices.inl"
} // namespace

static const std::filesystem::path script_settings_path =
    std::filesystem::path(ASSETS_ROOT_PATH) / "editor" / "script_path.txt";

auto make_app() -> std::unique_ptr<Dockforge> {
  return std::make_unique<Dockforge>();
}

Dockforge::~Dockforge() = default;

auto Dockforge::configure_window_hints() -> void {
  glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
}

auto Dockforge::init(const InitialisationContext &ctx) -> void {
  renderer =
      std::make_unique<SceneRenderer>(ctx.context, ctx.swapchain_resources);
  asset_loader = std::make_unique<AssetLoader>(*renderer);
  context = &ctx.context;
  NFD_SetDisplayPropertiesFromGLFW();

  script_engine = ScriptEngine::create();

  std::filesystem::path path_to_load;
  if (std::ifstream ifs{script_settings_path}; ifs) {
    std::string line;
    if (std::getline(ifs, line) && !line.empty())
      path_to_load = line;
  }
  if (path_to_load.empty() || !std::filesystem::exists(path_to_load)) {
    path_to_load =
        std::filesystem::path(ASSETS_ROOT_PATH) / "scripts" / "sandbox.lua";
  }
  load_script(VFS::get().mount_file("script", path_to_load));

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
    renderer->update_bloom_chain(viewport_resources.bloom_chain);
    renderer->update_bloom_scratch(viewport_resources.bloom_scratch);
    renderer->initialise_bindless();
  }
  {
    auto cube =
        mesh::load_from_memory(*renderer, cube_verts, cube_indices,
                               NullableVFSPath::create("engine://cube"));

    auto capsule =
        mesh::load_from_memory(*renderer, capsule_verts, capsule_indices,
                               NullableVFSPath::create("engine://capsule"));

    if (cube && capsule && cube->valid() && capsule->valid()) {
      info("Created engine meshes");
    }

    auto &concrete = static_cast<AssetLoader &>(*asset_loader);
    concrete.mesh_cache.try_emplace("engine://cube", std::move(cube.value()));
    concrete.mesh_cache.try_emplace("engine://capsule",
                                    std::move(capsule.value()));
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
    // Alpha blend: same shader, alpha blending enabled, no culling (foliage/glass
    // is two-sided), depth test only (no write — opaque prepass owns the depth buffer).
    // VK_CULL_MODE_NONE is static (not dynamic) so it overrides the global dynamic
    // cull-mode set before the opaque forward pass.
    auto result = registry.create_graphics({
        .shader_path = VFSPath::create("shaders://forward.slang"),
        .layout = renderer->pipeline_layout,
        .render_targets = {.color_formats = {VK_FORMAT_R16G16B16A16_SFLOAT},
                           .depth_format = VK_FORMAT_D32_SFLOAT},
        .cull_mode = VK_CULL_MODE_NONE,
        .samples = VK_SAMPLE_COUNT_4_BIT,
        .depth = {.test = true,
                  .write = false,
                  .compare_op = VK_COMPARE_OP_GREATER_OR_EQUAL},
        .blending = {BlendMode::alpha()},
        .extra_dynamic_states = {VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
                                 VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
                                 VK_DYNAMIC_STATE_FRONT_FACE},
    });
    if (!result) {
      error("forward blend pipeline: {}", result.error());
      std::abort();
    }
    forward_blend_pipeline = *result;
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

  constexpr std::string_view sponza_path =
      "meshes://Sponza/MISSING_main_sponza.glb";
  if (auto loaded_sponza =
          asset_loader->load_mesh(VFSPath::create(sponza_path))) {
    auto sponza = active_scene->make("Sponza");
    auto &mc = sponza.emplace<Components::Mesh>();
    mc.handle = *loaded_sponza;
    mc.source_path = VFSPath::create(sponza_path);
    sponza.get<Components::Transform>().mut().position = {-10, 3, 9};
  }

  if (script_engine && script_engine->loaded())
    script_engine->on_scene_load(editor_scene.get(), *asset_loader);

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

  renderer->initialise_settings();

  panels.push_back(std::make_unique<SceneOutlinerPanel>());
  panels.push_back(std::make_unique<InspectorPanel>());
  panels.push_back(
      std::make_unique<RendererSettingsPanel>(renderer->settings_registry));
  panels.push_back(std::make_unique<TexturePoolPanel>());
  {
    IPanel *pool_panel = panels.back().get();
    renderer->settings_registry.add("Texture Upload Pool", [pool_panel] {
      ImGui::Checkbox("Show panel", &pool_panel->open);
    });
  }
  renderer->settings_registry.add("Physics", [this] {
    ImGui::Checkbox("Show colliders", &show_collider_debug);
  });

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

auto Dockforge::load_script(const VFSPath &path) -> void {
  script_engine->stop_watching();
  if (auto result = script_engine->load(path)) {
    script_path = path;
    script_engine->start_watching();
    if (editor_scene)
      script_engine->on_scene_load(editor_scene.get(), *asset_loader);
  } else {
    warn("[ScriptEngine] {}", result.error());
  }
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

auto Dockforge::destroy() -> void {
  if (!sim_state.in<sim::S::Editing>())
    stop();

  if (script_engine) {
    script_engine->stop_watching();
    script_engine.reset();
  }

  imgui_renderer.reset();
  canvas_renderer.reset();

  viewport_resources.destroy(*context);
  renderer->destroy();

  editor_camera.reset();
  editor_scene.reset();
  runtime_scene.reset();
  renderer.reset();
}

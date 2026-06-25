#pragma once

#include <dockforge/editor_actions.hpp>
#include <dockforge/editor_state.hpp>
#include <dockforge/ipanel.hpp>
#include <dockforge/sim_state.hpp>
#include <dockyard/app.hpp>
#include <dockyard/bindless_handle.hpp>
#include <dockyard/canvas_renderer.hpp>
#include <dockyard/freelist_pool.hpp>
#include <dockyard/scene.hpp>
#include <dockyard/script_engine.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>


namespace dy {
class ImGuiRenderer;
struct SceneRenderer;
struct IAssetLoader;
} // namespace dy

using namespace dy;

struct EditorCamera;

// Mirrors ImGuizmo::MODE — cast to that type at call sites.
enum class GizmoMode : u8 { Local, World };

// Mirrors ImGuizmo::OPERATION bit values — cast to that type at call sites.
enum class GizmoOp : u32 {
  None         = 0,
  TranslateX   = 0x001,
  TranslateY   = 0x002,
  TranslateZ   = 0x004,
  RotateX      = 0x008,
  RotateY      = 0x010,
  RotateZ      = 0x020,
  RotateScreen = 0x040,
  ScaleX       = 0x080,
  ScaleY       = 0x100,
  ScaleZ       = 0x200,
  Translate    = 0x007,
  Rotate       = 0x078,
  Scale        = 0x380,
};
MAKE_BITFIELD(GizmoOp)

struct ShadowMapState {
  glm::mat4 last_view_matrix{1.0F};
  bool invalid = true;
  float near_plane = 0.5F;
  float far_plane = 100.0F;
};

struct Dockforge : App {
  std::shared_ptr<Scene> editor_scene;
  std::shared_ptr<Scene> runtime_scene;
  Scene *active_scene;

  GLFWwindow *main_window = nullptr;

  PipelineHandle forward_pipeline{};
  PipelineHandle depth_pipeline{};

  VulkanContext *context{};
  std::unique_ptr<EditorCamera> editor_camera;
  std::unique_ptr<ImGuiRenderer> imgui_renderer;
  std::unique_ptr<CanvasRenderer> canvas_renderer;
  std::unique_ptr<SceneRenderer> renderer;
  ViewportResources viewport_resources;

  std::optional<glm::vec2> pending_pick;
  GizmoOp gizmo_op = GizmoOp::Translate;
  GizmoMode gizmo_mode = GizmoMode::Local; // only applied when gizmo_op == Translate
  VkExtent2D viewport_panel_extent{};
  VkExtent2D viewport_panel_offset{};
  VkExtent2D last_ui_size{};
  VkExtent2D last_ui_offset{};
  std::optional<VkExtent2D> pending_viewport_resize{std::nullopt};
  std::optional<VkExtent2D> candidate_viewport_resize{std::nullopt};
  EditorState editor_state;
  EditorActions editor_actions;
  ShadowMapState shadow_map_state;

  enum class FullscreenMode : u8 { borderless, exclusive };
  bool is_fullscreen = false;
  FullscreenMode fullscreen_mode = FullscreenMode::borderless;
  int windowed_pos_x = 0;
  int windowed_pos_y = 0;
  int windowed_width = 0;
  int windowed_height = 0;

  double last_resize_change_time = 0.0;
  static constexpr double resize_debounce_delay = 0.1;
  bool viewport_hovered = false;

  std::unique_ptr<dy::ScriptEngine> script_engine;
  std::unique_ptr<dy::IAssetLoader> asset_loader;
  sim::Machine sim_state{sim::Editing{}};
  std::filesystem::path script_path;

  TextureHandle icon_play;
  TextureHandle icon_pause;
  TextureHandle icon_stop;
  TextureHandle icon_step;
  TextureHandle icon_reload;

  std::vector<std::unique_ptr<IPanel>> panels;

  ~Dockforge() override;

  auto toggle_fullscreen(FullscreenMode mode = FullscreenMode::borderless,
                         int monitor_index = -1) -> void;
  auto list_monitors() -> std::vector<std::string>;
  auto on_changed_tag(entt::registry &, entt::entity) -> void {}
  auto init(const InitialisationContext &ctx) -> void override;
  auto on_mouse_moved(const events::MouseMoved &e) -> void override;
  auto on_key_released(const events::KeyReleased &e) -> void override;
  auto on_mouse_scrolled(const events::MouseScrolled &) -> void override;
  [[nodiscard]] auto resolve_camera() const -> std::pair<glm::mat4, glm::mat4>;
  [[nodiscard]] auto resolve_camera_with_position() const
      -> std::tuple<glm::mat4, glm::mat4, glm::vec3>;
  auto resize(u32 w, u32 h) -> void override;

  auto try_pick_entity(glm::vec2 mouse_screen) -> void;
  auto build_ui() -> void;
  auto draw_debug_shapes() -> void;
  auto duplicate_entity(Entity) -> Entity;
  auto flush_material_overrides() -> void;
  auto patch_material_override_slots(u32 delta) -> void;
  auto draw_hdr_selector() -> void;

  auto configure_window_hints() -> void override;
  auto destroy() -> void override;
  auto update(float ts) -> void override;
  auto render(RenderContext &ctx) -> u64 override;

private:
  auto play() -> void;
  auto pause() -> void;
  auto resume() -> void;
  auto stop() -> void;

  void animation_state_update(float ts);

  auto step() -> void;
  auto load_script(const std::filesystem::path &) -> void;
  auto load_toolbar_icons() -> void;
  auto draw_toolbar() -> void;
  auto draw_titlebar() -> void;
};

auto make_app() -> std::unique_ptr<Dockforge>;

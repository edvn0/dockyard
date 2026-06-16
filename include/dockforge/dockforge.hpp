#pragma once

#include <dockforge/editor_actions.hpp>
#include <dockforge/editor_state.hpp>
#include <dockforge/ipanel.hpp>
#include <dockforge/sim_state.hpp>
#include <dockyard/app.hpp>
#include <dockyard/bindless_handle.hpp>
#include <dockyard/canvas_renderer.hpp>
#include <dockyard/freelist_pool.hpp>
#include <dockyard/game_dll.hpp>
#include <dockyard/game_memory.hpp>
#include <dockyard/scene.hpp>

#include <memory>
#include <string>
#include <vector>

// Should be able to remove these includes if I wrap the gizmo operation
#include <imgui.h>

#include <ImGuizmo.h>

namespace dy {
class ImGuiRenderer;
struct SceneRenderer;
struct IAssetLoader;
} // namespace dy

using namespace dy;

struct EditorCamera;

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

  MeshAssetHandle cube_mesh_handle{};
  PipelineHandle forward_pipeline{};
  PipelineHandle depth_pipeline{};

  VulkanContext *context{};
  std::unique_ptr<EditorCamera> editor_camera;
  std::unique_ptr<ImGuiRenderer> imgui_renderer;
  std::unique_ptr<CanvasRenderer> canvas_renderer;
  std::unique_ptr<SceneRenderer> renderer;
  ViewportResources viewport_resources;

  std::optional<glm::vec2> pending_pick;
  ImGuizmo::OPERATION gizmo_op = ImGuizmo::TRANSLATE;
  VkExtent2D viewport_panel_extent{};
  VkExtent2D viewport_panel_offset{};
  VkExtent2D last_ui_size{};
  VkExtent2D last_ui_offset{};
  std::optional<VkExtent2D> pending_viewport_resize{std::nullopt};
  std::optional<VkExtent2D> candidate_viewport_resize{std::nullopt};
  EditorState editor_state;
  EditorActions editor_actions;
  ShadowMapState shadow_map_state;

  double last_resize_change_time = 0.0;
  static constexpr double resize_debounce_delay = 0.1;

  std::unique_ptr<dy::GameDll>       game_dll;
  std::unique_ptr<dy::IAssetLoader>  asset_loader;
  dy::GameMemory                     game_memory;
  sim::Machine                       sim_state{sim::Editing{}};
  std::string                        game_dll_stem = "sandbox";

  TextureHandle icon_play;
  TextureHandle icon_pause;
  TextureHandle icon_stop;
  TextureHandle icon_step;
  TextureHandle icon_reload;

  std::vector<std::unique_ptr<IPanel>> panels;

  ~Dockforge() override;

  auto on_changed_tag(entt::registry &, entt::entity) -> void {}
  auto init(const InitialisationContext &ctx) -> void override;
  auto on_mouse_moved(const events::MouseMoved &e) -> void override;
  auto on_key_released(const events::KeyReleased& e) -> void override;
  auto on_mouse_scrolled(const events::MouseScrolled&) -> void override;
  [[nodiscard]] auto resolve_camera() const -> std::pair<glm::mat4, glm::mat4>;
  [[nodiscard]] auto resolve_camera_with_position() const
      -> std::tuple<glm::mat4, glm::mat4, glm::vec3>;
  auto resize(u32 w, u32 h) -> void override;

  auto try_pick_entity(glm::vec2 mouse_screen) -> void;
  auto build_ui() -> void;
  auto draw_debug_shapes() -> void;
  auto duplicate_entity(Entity) -> Entity;
  auto flush_material_overrides() -> void;
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
  auto step() -> void;
  auto load_toolbar_icons() -> void;
  auto draw_toolbar() -> void;
  auto draw_titlebar() -> void;
};

auto make_app() -> std::unique_ptr<Dockforge>;

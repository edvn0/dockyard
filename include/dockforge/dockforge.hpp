#pragma once

#include <dockforge/scene_outlier.hpp>
#include <dockyard/app.hpp>
#include <dockyard/scene.hpp>

#include <memory>

// Should be able to remove these includes if I wrap the gizmo operation
#include <imgui.h>

#include <ImGuizmo.h>

namespace dy {
class ImGuiRenderer;
struct SceneRenderer;
} // namespace dy

using namespace dy;

struct EditorCamera;

struct Dockforge : App {
  std::shared_ptr<Scene> editor_scene;
  std::shared_ptr<Scene> runtime_scene;
  std::shared_ptr<Scene> active_scene;

  GLFWwindow *main_window = nullptr;

  MeshHandle mesh_handle{};
  PipelineHandle forward_pipeline{};
  PipelineHandle depth_pipeline{};

  VulkanContext *context{};
  std::unique_ptr<EditorCamera> editor_camera;
  std::unique_ptr<ImGuiRenderer> imgui_renderer;
  std::unique_ptr<SceneRenderer> renderer;
  ViewportResources viewport_resources;

  std::optional<glm::vec2> pending_pick{};
  ImGuizmo::OPERATION gizmo_op = ImGuizmo::TRANSLATE;
  VkExtent2D viewport_panel_extent{};
  VkExtent2D viewport_panel_offset{};
  VkExtent2D last_ui_size{};
  VkExtent2D last_ui_offset{};
  std::optional<VkExtent2D> pending_viewport_resize{std::nullopt};
  std::optional<VkExtent2D> candidate_viewport_resize{std::nullopt};
  SceneOutlinerState state;

  double last_resize_change_time = 0.0;
  static constexpr double resize_debounce_delay = 0.1;

  ~Dockforge() override;

  auto on_changed_tag(entt::registry &, entt::entity) -> void {}
  auto init(const InitialisationContext &ctx) -> void override;
  auto on_mouse_moved(const events::mouse_moved &e) -> void override;
  auto on_key_released(const events::key_released &e) -> void override;
  [[nodiscard]] auto resolve_camera() const -> std::pair<glm::mat4, glm::mat4>;
  auto resize(u32 w, u32 h) -> void override;
  auto try_pick_entity(glm::vec2 mouse_screen) -> void;
  void refresh_entity_cache();
  void draw_scene_outliner();
  auto build_ui() -> void;
  auto destroy() -> void override;
  auto update(float ts) -> void override;
  auto render(RenderContext &ctx) -> u64 override;
};

auto make_app() -> std::unique_ptr<Dockforge>;
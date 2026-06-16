#pragma once

#include <dockforge/ipanel.hpp>
#include <dockforge/scene_outlier.hpp>
#include <entt/entt.hpp>

struct SceneOutlinerPanel final : IPanel {
  auto draw(EditorState &state, const EditorActions &actions) -> void override;
  [[nodiscard]] auto name() const -> std::string_view override {
    return "Entities";
  }

  auto on_stop() -> void override;

private:
  auto refresh_cache(EditorState &state) -> void;

  SceneOutlinerState cache;
  entt::entity pending_duplicate = entt::null;
  entt::entity pending_delete = entt::null;
  entt::entity delete_candidate = entt::null;
};

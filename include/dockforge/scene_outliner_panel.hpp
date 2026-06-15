#pragma once

#include <dockforge/ipanel.hpp>
#include <dockforge/scene_outlier.hpp>
#include <dockyard/scene.hpp>
#include <entt/entt.hpp>
#include <functional>

struct SceneOutlinerPanel final : IPanel {
    explicit SceneOutlinerPanel(const std::function<dy::Entity(dy::Entity)>& on_duplicate);

    auto draw(EditorState& state) -> void override;
    [[nodiscard]] auto name() const -> std::string_view override { return "Entities"; }

private:
    auto refresh_cache(EditorState& state) -> void;

    SceneOutlinerState                    cache;
    std::function<dy::Entity(dy::Entity)> on_duplicate;
    entt::entity pending_duplicate = entt::null;
    entt::entity pending_delete    = entt::null;
    entt::entity delete_candidate  = entt::null;
};

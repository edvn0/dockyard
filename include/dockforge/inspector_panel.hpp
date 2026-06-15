#pragma once

#include <dockforge/ipanel.hpp>

struct InspectorPanel final : IPanel {
    auto draw(EditorState& state) -> void override;
    [[nodiscard]] auto name() const -> std::string_view override { return "Inspector"; }
};

#include <dockforge/inspector_panel.hpp>
#include <dockforge/component_inspector.hpp>
#include <dockforge/component_renderers.hpp>
#include <dockforge/editor_state.hpp>
#include <dockyard/components.hpp>
#include <dockyard/scene.hpp>
#include <imgui.h>

using namespace dy;

auto InspectorPanel::draw(EditorState& state) -> void {
    if (!ImGui::Begin("Inspector")) {
        ImGui::End();
        return;
    }

    if (state.selected == entt::null) {
        ImGui::TextDisabled("Nothing selected");
        ImGui::End();
        return;
    }

    Entity entity{*state.active_scene, state.selected};
    if (!entity.valid()) {
        state.selected = entt::null;
        ImGui::End();
        return;
    }

    if (auto* tag = entity.try_get<Components::Tag>()) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%.*s",
                      static_cast<int>(tag->tag.size()), tag->tag.data());
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {6.F, 5.F});
        ImGui::SetNextItemWidth(-1.F);
        if (ImGui::InputText("##tag", buf, sizeof(buf)))
            tag->tag = buf;
        ImGui::PopStyleVar();
    }

    ImGui::Separator();
    ImGui::Spacing();
    if (ComponentInspector::draw(*state.renderer, entity))
        state.hierarchy_dirty = true;

    ImGui::End();
}

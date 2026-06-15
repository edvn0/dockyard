#include <dockforge/scene_outliner_panel.hpp>
#include <dockforge/editor_actions.hpp>
#include <dockforge/editor_state.hpp>
#include <dockyard/components.hpp>
#include <dockyard/scene.hpp>
#include <dockyard/scene_renderer.hpp>
#include <dockyard/types.hpp>
#include <imgui.h>
#include <tracy/Tracy.hpp>

using namespace dy;

auto SceneOutlinerPanel::refresh_cache(EditorState& state) -> void {
    ZoneScopedNC("SceneOutlinerPanel::refresh_cache", 0x808080);
    cache.entity_cache.clear();
    auto& registry = state.active_scene->registry();

    std::vector<entt::entity> roots;
    auto tag_view = registry.view<Components::Tag>();
    for (auto entity : tag_view) {
        if (!registry.any_of<Components::ParentOf>(entity))
            roots.push_back(entity);
    }

    auto& flat   = cache.entity_cache;
    auto children = registry.view<Components::ParentOf>();

    auto const add_recursive = [&](this auto&& self,
                                   entt::entity current,
                                   u32 depth) -> void {
        bool has_children = false;
        for (auto child : children) {
            if (children.get<Components::ParentOf>(child).parent == current) {
                has_children = true;
                break;
            }
        }
        flat.push_back({.entity = current, .depth = depth, .is_visible = true});
        if (has_children && cache.expanded_entities.contains(current)) {
            for (auto child : children) {
                if (children.get<Components::ParentOf>(child).parent == current)
                    self(child, depth + 1);
            }
        }
    };

    for (auto root : roots)
        add_recursive(root, 0);

    cache.cache_dirty = false;
    state.cache_dirty = false;
}

auto SceneOutlinerPanel::draw(EditorState& state, const EditorActions& actions) -> void {
    if (state.cache_dirty || cache.cache_dirty)
        refresh_cache(state);

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
    clipper.Begin(static_cast<int>(cache.entity_cache.size()), item_height);

    auto& registry  = state.active_scene->registry();
    auto child_view = registry.view<Components::ParentOf>();

    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const auto& cached = cache.entity_cache[static_cast<usize>(i)];
            Entity entity{*state.active_scene, cached.entity};

            auto* tag  = entity.try_get<Components::Tag>();
            auto* mesh = entity.try_get<Components::Mesh>();

            if (tag == nullptr)
                continue;

            const std::string_view label = tag->tag;
            if (!filter.PassFilter(label.data()))
                continue;

            if (cached.depth > 0)
                ImGui::Indent(static_cast<float>(cached.depth) * 16.0F);

            bool has_children = false;
            for (auto child : child_view) {
                if (child_view.get<Components::ParentOf>(child).parent == cached.entity) {
                    has_children = true;
                    break;
                }
            }

            const bool is_expanded = cache.expanded_entities.contains(cached.entity);
            ImGui::PushID(static_cast<int>(static_cast<uint32_t>(cached.entity)));

            if (has_children) {
                const ImGuiDir arrow_dir = is_expanded ? ImGuiDir_Down : ImGuiDir_Right;
                if (ImGui::ArrowButton("##toggle", arrow_dir)) {
                    if (is_expanded)
                        cache.expanded_entities.erase(cached.entity);
                    else
                        cache.expanded_entities.insert(cached.entity);
                    cache.cache_dirty = true;
                }
                ImGui::SameLine();
            } else {
                ImGui::Dummy(ImVec2(ImGui::GetFrameHeight(), 0));
                ImGui::SameLine();
            }

            std::array<char, 128> row_label{};
            if (mesh != nullptr) {
                std::snprintf(row_label.data(), row_label.size(), "[M] %.*s  (%u)",
                              static_cast<int>(label.size()), label.data(),
                              mesh->handle.index());
            } else {
                std::snprintf(row_label.data(), row_label.size(), "%.*s",
                              static_cast<int>(label.size()), label.data());
            }

            const bool is_selected = (state.selected == cached.entity);
            if (ImGui::Selectable(row_label.data(), is_selected,
                                  ImGuiSelectableFlags_SpanAllColumns))
                state.selected = cached.entity;

            if (ImGui::BeginPopupContextItem("##entity_ctx")) {
                if (ImGui::MenuItem("Duplicate"))
                    pending_duplicate = cached.entity;

                ImGui::Separator();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.9F, 0.35F, 0.35F, 1.F});
                if (ImGui::MenuItem("Delete"))
                    delete_candidate = cached.entity;
                ImGui::PopStyleColor();

                ImGui::EndPopup();
            }

            ImGui::PopID();

            if (cached.depth > 0)
                ImGui::Unindent(static_cast<float>(cached.depth) * 16.0F);

            if (is_selected)
                ImGui::SetItemDefaultFocus();
        }
    }
    clipper.End();

    if (pending_duplicate != entt::null) {
        actions.duplicate_entity({*state.active_scene, pending_duplicate});
        pending_duplicate = entt::null;
        cache.cache_dirty = true;
    }
    if (pending_delete != entt::null) {
        if (state.selected == pending_delete)
            state.selected = entt::null;
        actions.destroy_entity({*state.active_scene, pending_delete});
        pending_delete = entt::null;
    }

    if (!ImGui::GetIO().WantTextInput &&
        state.selected != entt::null &&
        ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
        delete_candidate = state.selected;
    }

    ImGui::EndChild();

    if (delete_candidate != entt::null)
        ImGui::OpenPopup("Delete Entity?");

    if (ImGui::BeginPopupModal("Delete Entity?", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Are you sure? This deletes all child entities too.");
        ImGui::Separator();
        if (ImGui::Button("OK", ImVec2(120, 0))) {
            pending_delete   = delete_candidate;
            delete_candidate = entt::null;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            delete_candidate = entt::null;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::End();
}

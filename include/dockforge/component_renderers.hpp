#pragma once

#include <dockforge/component_inspector.hpp>
#include <dockyard/component_traits.hpp>
#include <dockyard/components.hpp>
#include <dockyard/scene_renderer.hpp>

#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <imgui.h>

[[nodiscard]] auto draw_material_editor(dy::GPUMaterial &mat) -> bool;

template <>
struct ComponentRenderer<dy::Components::Transform>
    : public BaseComponentRenderer<
          ComponentRenderer<dy::Components::Transform>> {
  static constexpr std::string_view label = "Transform";
  static constexpr bool removable = false;
  static constexpr bool addable = false;

  static auto draw(dy::Components::Transform &t, dy::SceneRenderer &,
                   dy::Entity &) -> bool {
    bool changed = false;
    auto &&[pos, rot, scale] = t.get();

    glm::vec3 p = pos;
    glm::vec3 e = glm::degrees(glm::eulerAngles(rot));
    glm::vec3 s = scale;

    if (ImGui::DragFloat3("Position", glm::value_ptr(p), 0.01F)) {
      t.mut().position = p;
      changed = true;
    }
    if (ImGui::DragFloat3("Rotation", glm::value_ptr(e), 0.25F)) {
      t.mut().rotation = glm::quat(glm::radians(e));
      changed = true;
    }
    if (ImGui::DragFloat3("Scale", glm::value_ptr(s), 0.01F)) {
      t.mut().scale = s;
      changed = true;
    }

    return changed;
  }
};

// ── Mesh ─────────────────────────────────────────────────────────────────────

template <>
struct ComponentRenderer<dy::Components::Mesh>
    : public BaseComponentRenderer<ComponentRenderer<dy::Components::Mesh>> {
  static constexpr std::string_view label = "Mesh";
  static constexpr bool removable = true;
  static constexpr bool addable = true;

  // Helper to recursively draw the glTF node hierarchy
  static auto draw_node(const dy::MeshAsset &asset, dy::i32 node_idx) -> void {
    if (node_idx < 0 || node_idx >= static_cast<dy::i32>(asset.nodes.size()))
      return;

    const auto &node = asset.nodes[node_idx];

    // Create a unique string ID for ImGui's ID stack using the node index
    std::string node_label =
        node.name.empty() ? "Node #" + std::to_string(node_idx)
                          : node.name + " (#" + std::to_string(node_idx) + ")";

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow;
    if (node.primitives.empty()) {
      flags |= ImGuiTreeNodeFlags_Leaf; // Leaf nodes with no geometry
    }

    if (ImGui::TreeNodeEx((void *)(intptr_t)node_idx, flags, "%s",
                          node_label.c_str())) {
      // 1. Render Primitives within this Node
      if (!node.primitives.empty()) {
        if (ImGui::TreeNode("Primitives")) {
          for (size_t p = 0; p < node.primitives.size(); ++p) {
            const auto &prim = node.primitives[p];
            const auto &lod_group = prim.lod_group;

            if (ImGui::TreeNode((void *)(intptr_t)p,
                                "Primitive %zu (Mat ID: %u)", p,
                                prim.material_id)) {
              ImGui::Text("LOD Count: %u", lod_group.lod_count);
              ImGui::Text("Vertex Offset: %d", lod_group.vertex_offset);

              // Draw fixed-size array LODs up to max_lods or lod_count
              if (ImGui::TreeNode("LOD Levels")) {
                auto active_lods = std::min(lod_group.lod_count,
                                            static_cast<dy::u8>(dy::max_lods));
                for (dy::u8 l = 0; l < active_lods; ++l) {
                  ImGui::BulletText("LOD %u: Index Count: %u | First Index: %u",
                                    l, lod_group.lods[l].index_count,
                                    lod_group.lods[l].first_index);
                }
                ImGui::TreePop();
              }

              ImGui::TreePop();
            }
          }
          ImGui::TreePop();
        }
      }

      // 2. Recursively render child nodes
      // Loop through all nodes to find ones whose parent_index matches current
      // node_idx
      for (size_t i = 0; i < asset.nodes.size(); ++i) {
        if (asset.nodes[i].parent_index == node_idx) {
          draw_node(asset, static_cast<dy::i32>(i));
        }
      }

      ImGui::TreePop();
    }
  }

  static auto draw(dy::Components::Mesh &m, dy::SceneRenderer &scene_renderer,
                   dy::Entity &) -> bool {
    bool modified = false;
    auto &registry = scene_renderer.mesh_registry;

    // Handle Asset Selection Combo Box
    if (ImGui::TreeNodeEx("Asset Selection", ImGuiTreeNodeFlags_DefaultOpen)) {
      auto current_handle = m.handle;
      std::string preview_name =
          current_handle.valid()
              ? "Mesh Asset #" + std::to_string(current_handle.index())
              : "None (Empty Handle)";

      if (ImGui::BeginCombo("Bound Mesh", preview_name.c_str())) {
        if (ImGui::Selectable("None (Empty)", current_handle.empty())) {
          m.handle = {};
          modified = true;
        }

        auto slots = registry.data();
        for (dy::u32 idx = 0u; idx < slots.size(); ++idx) {
          if (registry.is_live(idx)) {
            auto live_handle = registry.handle_at(idx);
            std::string label = "Mesh Asset #" + std::to_string(idx);

            if (ImGui::Selectable(label.c_str(),
                                  current_handle == live_handle)) {
              m.handle = live_handle;
              modified = true;
            }
          }
        }
        ImGui::EndCombo();
      }
      ImGui::TreePop();
    }

    ImGui::Separator();

    // Structural Hierarchy Diagnostics
    if (m.handle.valid()) {
      if (auto *asset = registry.get(m.handle)) {
        if (ImGui::TreeNodeEx("Mesh Hierarchy Details",
                              ImGuiTreeNodeFlags_DefaultOpen)) {

          // Layout info
          ImGui::Text("Global Vertex Base Offset:  %zu",
                      asset->vertex_base_offset);
          ImGui::Text("Global Shadow Vert Offset: %zu",
                      asset->shadow_vertex_base_offset);
          ImGui::Text("Global Index Base Offset:   %zu",
                      asset->index_base_offset);
          ImGui::Text("Material Slots Count:       %zu",
                      asset->material_slots.size());

          ImGui::Spacing();

          // Run recursive node presentation starting at root nodes
          if (ImGui::TreeNodeEx("Scene Graph Nodes",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
            if (asset->root_node_indices.empty()) {
              ImGui::TextDisabled("No root nodes found.");
            } else {
              for (dy::u32 root_idx : asset->root_node_indices) {
                draw_node(*asset, static_cast<dy::i32>(root_idx));
              }
            }
            ImGui::TreePop();
          }

          ImGui::TreePop();
        }
      } else {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                           "Warning: Stale or Invalid Handle!");
      }
    } else {
      ImGui::TextDisabled("No mesh asset assigned to this component.");
    }

    return modified;
  }
};

// ── Material override ────────────────────────────────────────────────────────

template <>
struct ComponentRenderer<dy::Components::MaterialOverride>
    : public BaseComponentRenderer<
          ComponentRenderer<dy::Components::MaterialOverride>> {
  static constexpr std::string_view label = "Material Override";
  static constexpr bool removable = true;
  static constexpr bool addable = true;

  static auto draw(dy::Components::MaterialOverride &ov, dy::SceneRenderer &,
                   dy::Entity &) -> bool {
    const bool changed = draw_material_editor(ov.material);
    if (changed)
      ov.dirty = true;
    return changed;
  }

  static auto on_remove(dy::SceneRenderer &renderer, dy::Entity e) -> void {
    renderer.remove_override(e);
  }

  static auto on_add(dy::SceneRenderer &renderer, dy::Entity e) -> void {
    auto &ov = e.emplace<dy::Components::MaterialOverride>();
    if (auto *mesh = e.try_get<dy::Components::Mesh>()) {
      auto mats = renderer.get_material_view_mut(*mesh);
      if (!mats.empty())
        ov.material = mats.first();
    }
    ov.dirty = true;
  }
};

// ── Camera ───────────────────────────────────────────────────────────────────

template <>
struct ComponentRenderer<dy::Components::Camera>
    : public BaseComponentRenderer<ComponentRenderer<dy::Components::Camera>> {
  static constexpr std::string_view label = "Camera";
  static constexpr bool removable = true;
  static constexpr bool addable = true;

  static auto draw(dy::Components::Camera &cam, dy::SceneRenderer &,
                   dy::Entity &) -> bool {
    bool changed = false;
    float &fov = cam.fov_degrees;
    float &near = cam.near_plane;
    float &far = cam.far_plane;
    if (ImGui::SliderFloat("FOV", &fov, 1.F, 170.F)) {
      changed = true;
    }
    if (ImGui::SliderFloat("Near", &near, 0.001F, 10.F)) {
      changed = true;
    }
    if (ImGui::SliderFloat("Far", &far, 1.F, 10'000.F)) {
      changed = true;
    }
    return changed;
  }
};

// ── Debug frustum
// ─────────────────────────────────────────────────────────────

template <>
struct ComponentRenderer<dy::Components::DebugFrustum>
    : public BaseComponentRenderer<
          ComponentRenderer<dy::Components::DebugFrustum>> {
  static constexpr std::string_view label = "Debug Frustum";
  static constexpr bool removable = true;
  static constexpr bool addable = true;

  static auto draw(dy::Components::DebugFrustum &f, dy::SceneRenderer &,
                   dy::Entity &) -> bool {
    bool changed = false;
    changed |=
        ImGui::SliderFloat("FOV", &f.projection_config.fov_degrees, 1.F, 179.F);
    changed |=
        ImGui::SliderFloat("Aspect", &f.projection_config.aspect, 0.1F, 10.F);
    changed |=
        ImGui::SliderFloat("Near", &f.projection_config.near, 0.01F, 100.F);
    changed |=
        ImGui::SliderFloat("Far", &f.projection_config.far, 0.1F, 1000.F);
    changed |= ImGui::ColorEdit4("Color", glm::value_ptr(f.color));
    return changed;
  }
};

template <>
struct ComponentRenderer<dy::Components::ParentOf>
    : public BaseComponentRenderer<
          ComponentRenderer<dy::Components::ParentOf>> {
  static constexpr std::string_view label = "Parent";
  static constexpr bool removable = true;
  static constexpr bool addable = false;

  static auto draw(dy::Components::ParentOf &rel, dy::SceneRenderer &,
                   dy::Entity &e) -> bool {
    if (!e.parent_is_valid()) {
      ImGui::TextDisabled("(invalid parent)");
      return false;
    }
    if (auto *tag = e.try_get_for_parent<dy::Components::Tag>())
      ImGui::Text("%.*s", static_cast<int>(tag->tag.size()), tag->tag.data());
    else
      ImGui::Text("entity %u", static_cast<dy::u32>(rel.parent));
    return false;
  }
};

inline auto ComponentInspector::draw(dy::SceneRenderer &renderer,
                                     dy::Entity &entity) -> void {
  std::optional<std::function<void()>> pending_remove;
  std::optional<std::function<void()>> pending_add;

  dy::for_each_type<dy::MasterComponentList>([&]<typename T>() {
    if constexpr (dy::ComponentConfig<T>::ui_inspectable) {
      static_assert(has_valid_renderer<T>,
                    "Component is marked ui_inspectable but lacks a valid "
                    "ComponentRenderer specialization!");

      draw_one<T>(renderer, entity, pending_remove);
    }
  });

  draw_add_button(renderer, entity, pending_add);

  if (pending_remove)
    (*pending_remove)();
  if (pending_add)
    (*pending_add)();
}

template <typename T>
inline auto ComponentInspector::draw_one(
    dy::SceneRenderer &renderer, dy::Entity &entity,
    std::optional<std::function<void()>> &pending_remove) -> void {
  if constexpr (!dy::ComponentConfig<T>::ui_inspectable) {
    return;
  }

  using R = ComponentRenderer<T>;
  auto *comp = entity.try_get<T>();
  if (comp == nullptr)
    return;

  ImGui::PushID(R::label.data());
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{6.F, 5.F});

  // Reserve space for the × button inside the header
  const float available = ImGui::GetContentRegionAvail().x;
  const float btn_w =
      R::removable
          ? (ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x * 2.F)
          : 0.F;

  constexpr ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Framed |
                                       ImGuiTreeNodeFlags_SpanAvailWidth |
                                       ImGuiTreeNodeFlags_AllowOverlap;

  const bool open = ImGui::TreeNodeEx("##node", flags, "%s", R::label.data());

  // × remove button — aligned to the right inside the header row
  if constexpr (R::removable) {
    ImGui::SameLine(available - btn_w);
    ImGui::PushStyleColor(ImGuiCol_Button, {0.55F, 0.13F, 0.13F, 1.F});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.72F, 0.18F, 0.18F, 1.F});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, {0.88F, 0.22F, 0.22F, 1.F});
    if (ImGui::SmallButton("×")) {
      pending_remove = [&, entity]() mutable {
        if constexpr (has_remove_hook<R>)
          R::on_remove(renderer, entity);
        else
          entity.template remove<T>();
      };
    }
    ImGui::PopStyleColor(3);
  }

  ImGui::PopStyleVar();

  if (open) {
    ImGui::PushItemWidth(-1.F);
    ImGui::Spacing();
    const bool changed = R::draw(*comp, renderer, entity);
    if (changed) {
      // if constexpr (std::is_same_v<T, dy::Components::Transform>)
      // app.state.hierarchy_dirty = true;
    }
    ImGui::Spacing();
    ImGui::PopItemWidth();
    ImGui::TreePop();
  }

  ImGui::PopID();
}

inline auto ComponentInspector::draw_add_button(
    dy::SceneRenderer &renderer, dy::Entity &entity,
    std::optional<std::function<void()>> &pending_add) -> void {
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
  ImGui::PushStyleColor(ImGuiCol_Button, {0.18F, 0.38F, 0.18F, 1.F});
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.22F, 0.50F, 0.22F, 1.F});
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, {0.28F, 0.62F, 0.28F, 1.F});
  const bool clicked = ImGui::Button("  +  Add Component  ", {-1.F, 0.F});
  ImGui::PopStyleColor(3);

  if (clicked)
    ImGui::OpenPopup("##add_popup");

  if (ImGui::BeginPopup("##add_popup")) {
    dy::for_each_type<dy::MasterComponentList>([&]<typename T>() {
      if constexpr (dy::ComponentConfig<T>::ui_inspectable) {
        using R = ComponentRenderer<T>;
        if constexpr (!R::addable)
          return;
        if (entity.template try_get<T>() != nullptr)
          return;

        if (ImGui::MenuItem(R::label.data())) {
          pending_add = [&, entity]() mutable {
            if constexpr (has_add_hook<R>)
              R::on_add(renderer, entity);
            else
              entity.template emplace<T>();
          };
        }
      }
    });
    ImGui::EndPopup();
  }
}
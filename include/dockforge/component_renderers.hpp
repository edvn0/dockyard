#pragma once

#include "dockyard/mesh_loader.hpp"
#include <dockforge/component_inspector.hpp>
#include <dockyard/animation.hpp>
#include <dockyard/component_traits.hpp>
#include <dockyard/components.hpp>
#include <dockyard/scene_renderer.hpp>

#include <array>
#include <cstring>
#include <nfd.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <imgui.h>
#include <unordered_map>

[[nodiscard]] auto draw_material_editor(dy::GPUMaterial &mat) -> bool;

namespace {
auto labelled_input(std::string_view name, auto fn, auto &&...args) -> decltype(auto) {
  ImGui::TextUnformatted(name.data());
  const auto id = std::format("##{}", name);
  return fn(id.c_str(), std::forward<decltype(args)>(args)...);
}
} // namespace

template <>
struct ComponentRenderer<dy::Components::Transform>
    : public BaseComponentRenderer<
          ComponentRenderer<dy::Components::Transform>> {
  static constexpr std::string_view label = "Transform";
  static constexpr bool removable = false;
  static constexpr bool addable = false;

  static auto draw(dy::Components::Transform &t, dy::SceneRenderer &,
                   dy::Scene &, dy::Entity &) -> bool {
    bool changed = false;
    auto &&[pos, rot, scale] = t.get();

    glm::vec3 p = pos;
    glm::vec3 e = glm::degrees(glm::eulerAngles(rot));
    glm::vec3 s = scale;

    if (labelled_input("Position", [&](const char *id) { return ImGui::DragFloat3(id, glm::value_ptr(p), 0.01F); })) {
      t.mut().position = p;
      changed = true;
    }
    if (labelled_input("Rotation", [&](const char *id) { return ImGui::DragFloat3(id, glm::value_ptr(e), 0.25F); })) {
      t.mut().rotation = glm::quat(glm::radians(e));
      changed = true;
    }
    if (labelled_input("Scale", [&](const char *id) { return ImGui::DragFloat3(id, glm::value_ptr(s), 0.01F); })) {
      t.mut().scale = s;
      changed = true;
    }

    return changed;
  }
};

template <>
struct ComponentRenderer<dy::Components::Mesh>
    : public BaseComponentRenderer<ComponentRenderer<dy::Components::Mesh>> {
  static constexpr std::string_view label = "Mesh";
  static constexpr bool removable = true;
  static constexpr bool addable = true;

  static auto draw_node(const dy::MeshAsset &asset, dy::i32 node_idx) -> void {
    if (node_idx < 0 || node_idx >= static_cast<dy::i32>(asset.nodes.size()))
      return;

    const auto &node = asset.nodes[node_idx];

    std::string node_label =
        node.name.empty() ? "Node #" + std::to_string(node_idx)
                          : node.name + " (#" + std::to_string(node_idx) + ")";

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow;
    if (node.primitives.empty()) {
      flags |= ImGuiTreeNodeFlags_Leaf;
    }

    if (ImGui::TreeNodeEx((void *)(intptr_t)node_idx, flags, "%s",
                          node_label.c_str())) {
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

      for (size_t i = 0; i < asset.nodes.size(); ++i) {
        if (asset.nodes[i].parent_index == node_idx) {
          draw_node(asset, static_cast<dy::i32>(i));
        }
      }

      ImGui::TreePop();
    }
  }

  // Entities with a load in flight, mapped to the path being loaded — read
  // by this same inline (implicitly-inline) member function from every TU
  // that includes this header, so a function-local static is one shared map,
  // not one per TU. Cleared by the async on_complete below.
  static auto loading_meshes() -> std::unordered_map<entt::entity, std::string> & {
    static std::unordered_map<entt::entity, std::string> loading;
    return loading;
  }

  static auto draw(dy::Components::Mesh &m, dy::SceneRenderer &scene_renderer,
                   dy::Scene &scene, dy::Entity &entity) -> bool {
    bool modified = false;
    auto &registry = scene_renderer.mesh_registry;

    if (ImGui::TreeNodeEx("Asset Selection", ImGuiTreeNodeFlags_DefaultOpen)) {
      auto current_handle = m.handle;
      std::string preview_name =
          current_handle.valid()
              ? (m.source_path.valid() ? std::string{m.source_path.view()}
                                       : "Mesh Asset #" + std::to_string(current_handle.index()))
              : "None (Empty Handle)";

      const auto loading_it = loading_meshes().find(entity.id());
      const bool is_loading = loading_it != loading_meshes().end();

      if (is_loading) {
        ImGui::BeginDisabled();
        constexpr std::array<const char *, 4> spinner_frames = {"|", "/", "-", "\\"};
        const auto frame = static_cast<size_t>(ImGui::GetTime() * 8.0) %
                           spinner_frames.size();
        ImGui::Button(
            std::format("Loading {} {}...", spinner_frames[frame], loading_it->second)
                .c_str());
        ImGui::EndDisabled();
      } else if (ImGui::Button("Load new mesh")) {
      constexpr std::array<nfdfilteritem_t, 1> filters = {
          nfdfilteritem_t{.name = "Valid meshes", .spec = "dymesh,glb,zst,zstd,gz"},
      };

      NFD::UniquePath out_path;
      const nfdresult_t result = NFD::OpenDialog(
          out_path, filters.data(), std::size(filters));

      if (result == NFD_OKAY) {
          auto path = dy::VFS::get().mount_file("meshes", std::filesystem::path {out_path.get()});
          dy::info("OutPath: {}, VFSPath: {}", out_path.get(), path.view());

          loading_meshes()[entity.id()] = std::string{path.view()};

          auto &ent_registry = scene.registry();
          const entt::entity ent = entity.id();

          dy::mesh::load_from_path_async(
              path, scene_renderer,
              [&ent_registry, ent, path](dy::MeshAssetHandle handle) {
                loading_meshes().erase(ent);
                if (!ent_registry.valid(ent))
                  return;
                auto *mesh = ent_registry.try_get<dy::Components::Mesh>(ent);
                if (mesh == nullptr)
                  return;
                if (!handle.valid()) {
                  dy::warn("Async mesh load failed for '{}'", path.view());
                  return;
                }
                mesh->handle = handle;
                mesh->source_path = dy::NullableVFSPath{path};
              });
      } else if (result == NFD_ERROR) {
        dy::warn("Native file dialog failed: {}", NFD::GetError());
      }
      }

      ImGui::TextUnformatted("Bound Mesh");
      if (ImGui::BeginCombo("##BoundMesh", preview_name.c_str())) {
        if (ImGui::Selectable("None (Empty)", current_handle.empty())) {
          m.handle = {};
          m.source_path = {};
          modified = true;
        }

        auto slots = registry.data();
        for (dy::u32 idx = 0U; idx < slots.size(); ++idx) {
          if (registry.is_live(idx)) {
            auto live_handle = registry.handle_at(idx);
            const auto *live_asset = registry.get(live_handle);
            std::string mesh_label =
                ((live_asset != nullptr) && live_asset->source_path.valid())
                    ? std::string{live_asset->source_path.view()}
                    : "Mesh Asset #" + std::to_string(idx);

            if (ImGui::Selectable(mesh_label.c_str(),
                                  current_handle == live_handle)) {
              m.handle = live_handle;
              m.source_path = live_asset ? live_asset->source_path : dy::NullableVFSPath{};
              modified = true;
            }
          }
        }
        ImGui::EndCombo();
      }
      ImGui::TreePop();
    }

    ImGui::Separator();

    if (m.handle.valid()) {
      if (auto *asset = registry.get(m.handle)) {
        if (ImGui::TreeNodeEx("Mesh Hierarchy Details",
                              ImGuiTreeNodeFlags_DefaultOpen)) {

          ImGui::Text("Global Vertex Base Offset:  %zu",
                      asset->vertex_base_offset);
          ImGui::Text("Global Shadow Vert Offset: %zu",
                      asset->shadow_vertex_base_offset);
          ImGui::Text("Global Index Base Offset:   %zu",
                      asset->index_base_offset);
          ImGui::Text("Material Slots Count:       %zu",
                      asset->material_slots.size());

          ImGui::Spacing();

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
        ImGui::TextColored(ImVec4(1.F, 0.4f, 0.4f, 1.F),
                           "Warning: Stale or Invalid Handle!");
      }
    } else {
      ImGui::TextDisabled("No mesh asset assigned to this component.");
    }

    return modified;
  }
};

template <>
struct ComponentRenderer<dy::Components::MaterialOverride>
    : public BaseComponentRenderer<
          ComponentRenderer<dy::Components::MaterialOverride>> {
  static constexpr std::string_view label = "Material Override";
  static constexpr bool removable = true;
  static constexpr bool addable = true;

  static auto draw(dy::Components::MaterialOverride &ov, dy::SceneRenderer &,
                   dy::Scene &, dy::Entity &) -> bool {
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

template <>
struct ComponentRenderer<dy::Components::Camera>
    : public BaseComponentRenderer<ComponentRenderer<dy::Components::Camera>> {
  static constexpr std::string_view label = "Camera";

  static auto draw(dy::Components::Camera &cam, dy::SceneRenderer &,
                   dy::Scene &scene, dy::Entity &e) -> bool {
    bool changed = false;

    bool is_ortho = !cam.is_perspective;
    if (labelled_input("Orthographic", ImGui::Checkbox, &is_ortho)) {
      cam.is_perspective = !is_ortho;
      changed = true;
    }

    ImGui::Separator();

    if (cam.is_perspective) {
      changed |= labelled_input("FOV", [&](const char *id) { return ImGui::SliderFloat(id, &cam.fov_degrees, 1.F, 170.F); });
    } else {
      changed |= labelled_input("Left",   [&](const char *id) { return ImGui::DragFloat(id, &cam.ortho_left,   0.1F); });
      changed |= labelled_input("Right",  [&](const char *id) { return ImGui::DragFloat(id, &cam.ortho_right,  0.1F); });
      changed |= labelled_input("Top",    [&](const char *id) { return ImGui::DragFloat(id, &cam.ortho_top,    0.1F); });
      changed |= labelled_input("Bottom", [&](const char *id) { return ImGui::DragFloat(id, &cam.ortho_bottom, 0.1F); });
    }

    ImGui::Separator();

    changed |= labelled_input("Near", [&](const char *id) { return ImGui::SliderFloat(id, &cam.near_plane, 0.001F, 10.F); });
    changed |= labelled_input("Far",  [&](const char *id) { return ImGui::SliderFloat(id, &cam.far_plane,  1.F, 10'000.F); });

    bool is_primary = (&cam == scene.primary_camera());

    if (labelled_input("Primary Camera", ImGui::Checkbox, &is_primary)) {
      if (is_primary) {
        scene.set_primary_camera(e.handle());
      } else {
        scene.clear_primary_camera();
      }
      changed = true;
    }

    return changed;
  }
};

template <>
struct ComponentRenderer<dy::AnimationState>
    : public BaseComponentRenderer<ComponentRenderer<dy::AnimationState>> {
  static constexpr std::string_view label = "Animation State";
  static constexpr bool removable = true;
  static constexpr bool addable = false;

  static auto draw(dy::AnimationState &state, dy::SceneRenderer &renderer,
                   dy::Scene &, dy::Entity &e) -> bool {
    bool changed = false;

    const auto *mesh_comp = e.try_get<dy::Components::Mesh>();
    if (mesh_comp == nullptr || !mesh_comp->handle.valid()) {
      ImGui::TextDisabled("No mesh asset.");
      return false;
    }
    const auto *asset = renderer.get_mesh(mesh_comp->handle);
    if (asset == nullptr || asset->animations.empty()) {
      ImGui::TextDisabled("Mesh has no animation clips.");
      return false;
    }

    int current_idx = -1;
    for (int i = 0; i < static_cast<int>(asset->animations.size()); ++i) {
      if (state.clip == &asset->animations[i]) {
        current_idx = i;
        break;
      }
    }

    const char *preview = current_idx >= 0
                              ? asset->animations[current_idx].name.c_str()
                              : "(none)";
    ImGui::TextUnformatted("Clip");
    if (ImGui::BeginCombo("##clip", preview)) {
      for (int i = 0; i < static_cast<int>(asset->animations.size()); ++i) {
        const bool selected = (i == current_idx);
        if (ImGui::Selectable(asset->animations[i].name.c_str(), selected)) {
          state.update_animation(asset->animations[i]);
          state.time = 0.0F;
          changed = true;
        }
        if (selected)
          ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }

    if (state.clip != nullptr) {
      ImGui::Spacing();
      ImGui::Text("%.2f / %.2f s", state.time, state.clip->duration);
      changed |= labelled_input("Loop", ImGui::Checkbox, &state.loop);
    }

    return changed;
  }
};

template <>
struct ComponentRenderer<dy::Components::DebugFrustum>
    : public BaseComponentRenderer<
          ComponentRenderer<dy::Components::DebugFrustum>> {
  static constexpr std::string_view label = "Debug Frustum";
  static constexpr bool removable = true;
  static constexpr bool addable = true;

  static auto draw(dy::Components::DebugFrustum &f, dy::SceneRenderer &,
                   dy::Scene &, dy::Entity &) -> bool {
    bool changed = false;
    changed |= labelled_input("FOV",    [&](const char *id) { return ImGui::SliderFloat(id, &f.projection_config.fov_degrees, 1.F, 179.F); });
    changed |= labelled_input("Aspect", [&](const char *id) { return ImGui::SliderFloat(id, &f.projection_config.aspect,      0.1F, 10.F); });
    changed |= labelled_input("Near",   [&](const char *id) { return ImGui::SliderFloat(id, &f.projection_config.near_plane,  0.01F, 100.F); });
    changed |= labelled_input("Far",    [&](const char *id) { return ImGui::SliderFloat(id, &f.projection_config.far_plane,   0.1F, 1000.F); });
    changed |= labelled_input("Color",  [&](const char *id) { return ImGui::ColorEdit4(id, glm::value_ptr(f.color)); });
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
                   dy::Scene &, dy::Entity &e) -> bool {
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

template <>
struct ComponentRenderer<dy::Components::PointLight>
    : public BaseComponentRenderer<
          ComponentRenderer<dy::Components::PointLight>> {
  static constexpr std::string_view label = "Point Light";
  static constexpr bool removable = true;
  static constexpr bool addable = true;

  static auto draw(dy::Components::PointLight &light, dy::SceneRenderer &,
                   dy::Scene &, dy::Entity &) -> bool {
    bool changed = false;
    changed |= labelled_input("Color",     [&](const char *id) { return ImGui::ColorEdit3(id, glm::value_ptr(light.color)); });
    changed |= labelled_input("Intensity", [&](const char *id) { return ImGui::SliderFloat(id, &light.intensity, 0.F, 100.F); });
    changed |= labelled_input("Radius",    [&](const char *id) { return ImGui::SliderFloat(id, &light.radius, 0.1F, 100.F); });
    return changed;
  }

  static auto on_add(dy::SceneRenderer &, dy::Entity e) -> void {
    auto &light = e.emplace<dy::Components::PointLight>();
    light.color = {1.F, 1.F, 1.F};
    light.intensity = 1.F;
    light.radius = 10.F;
    // renderer.mark_point_lights_dirty();
  }
};

template <>
struct ComponentRenderer<dy::Components::FirstPersonController>
    : public BaseComponentRenderer<
          ComponentRenderer<dy::Components::FirstPersonController>> {
  static constexpr std::string_view label = "First Person Controller";
  static constexpr bool removable = true;
  static constexpr bool addable = true;

  static auto draw(dy::Components::FirstPersonController &ctrl,
                   dy::SceneRenderer &, dy::Scene &, dy::Entity &) -> bool {
    bool changed = false;
    changed |= labelled_input("Move Speed",  [&](const char *id) { return ImGui::SliderFloat(id, &ctrl.move_speed, 0.1F, 50.F); });
    changed |= labelled_input("Sensitivity", [&](const char *id) { return ImGui::SliderFloat(id, &ctrl.sensitivity, 0.01F, 1.F); });
    return changed;
  }

  static auto on_add(dy::SceneRenderer &, dy::Entity e) -> void {
    e.emplace<dy::Components::FirstPersonController>();
  }
};

template <>
struct ComponentRenderer<dy::Components::Collider>
    : public BaseComponentRenderer<ComponentRenderer<dy::Components::Collider>> {
  static constexpr std::string_view label = "Collider";
  static constexpr bool removable = true;
  static constexpr bool addable = true;

  static auto draw(dy::Components::Collider &collider, dy::SceneRenderer &,
                   dy::Scene &, dy::Entity &) -> bool {
    bool changed = false;

    static constexpr std::array<const char *, 4> shape_names{"Box", "Sphere",
                                                              "Capsule", "Mesh"};
    int shape_idx = static_cast<int>(collider.shape);
    ImGui::TextUnformatted("Shape");
    if (ImGui::Combo("##shape", &shape_idx, shape_names.data(),
                     static_cast<int>(shape_names.size()))) {
      collider.shape = static_cast<dy::Components::ColliderShape>(shape_idx);
      changed = true;
    }

    switch (collider.shape) {
    case dy::Components::ColliderShape::Box:
      changed |= labelled_input("Half Extents", [&](const char *id) {
        return ImGui::DragFloat3(id, glm::value_ptr(collider.half_extents), 0.01F,
                                 0.01F);
      });
      break;
    case dy::Components::ColliderShape::Sphere:
      changed |= labelled_input("Radius", [&](const char *id) {
        return ImGui::DragFloat(id, &collider.radius, 0.01F, 0.01F);
      });
      break;
    case dy::Components::ColliderShape::Capsule:
      changed |= labelled_input("Radius", [&](const char *id) {
        return ImGui::DragFloat(id, &collider.radius, 0.01F, 0.01F);
      });
      changed |= labelled_input("Height", [&](const char *id) {
        return ImGui::DragFloat(id, &collider.height, 0.01F, 0.01F);
      });
      break;
    case dy::Components::ColliderShape::Mesh: {
      char buf[256];
      const auto view = collider.mesh_source_path.view();
      std::snprintf(buf, sizeof(buf), "%.*s", static_cast<int>(view.size()),
                    view.data());
      ImGui::TextUnformatted("Mesh Source Path");
      if (ImGui::InputText("##mesh_source_path", buf, sizeof(buf))) {
        collider.mesh_source_path = std::strlen(buf) == 0
                                        ? dy::NullableVFSPath{}
                                        : dy::NullableVFSPath::create("{}", buf);
        changed = true;
      }
      break;
    }
    }

    return changed;
  }

  static auto on_add(dy::SceneRenderer &, dy::Entity e) -> void {
    e.emplace<dy::Components::Collider>();
  }
};

template <>
struct ComponentRenderer<dy::Components::RigidBody>
    : public BaseComponentRenderer<
          ComponentRenderer<dy::Components::RigidBody>> {
  static constexpr std::string_view label = "Rigid Body";
  static constexpr bool removable = true;
  static constexpr bool addable = true;

  static auto draw(dy::Components::RigidBody &body, dy::SceneRenderer &,
                   dy::Scene &, dy::Entity &) -> bool {
    bool changed = false;
    changed |= labelled_input("Mass",        [&](const char *id) { return ImGui::DragFloat(id, &body.mass, 0.1F, 0.F, 10'000.F); });
    changed |= labelled_input("Friction",    [&](const char *id) { return ImGui::SliderFloat(id, &body.friction, 0.F, 2.F); });
    changed |= labelled_input("Restitution", [&](const char *id) { return ImGui::SliderFloat(id, &body.restitution, 0.F, 1.F); });
    changed |= labelled_input("Kinematic",   ImGui::Checkbox, &body.kinematic);
    return changed;
  }

  static auto on_add(dy::SceneRenderer &, dy::Entity e) -> void {
    e.emplace<dy::Components::RigidBody>();
  }
};

template <>
struct ComponentRenderer<dy::Components::CharacterController>
    : public BaseComponentRenderer<
          ComponentRenderer<dy::Components::CharacterController>> {
  static constexpr std::string_view label = "Character Controller";
  static constexpr bool removable = true;
  static constexpr bool addable = true;

  static auto draw(dy::Components::CharacterController &ctrl,
                   dy::SceneRenderer &, dy::Scene &, dy::Entity &) -> bool {
    bool changed = false;
    changed |= labelled_input("Radius",      [&](const char *id) { return ImGui::DragFloat(id, &ctrl.radius, 0.01F, 0.01F, 5.F); });
    changed |= labelled_input("Height",      [&](const char *id) { return ImGui::DragFloat(id, &ctrl.height, 0.01F, 0.01F, 5.F); });
    changed |= labelled_input("Step Height", [&](const char *id) { return ImGui::DragFloat(id, &ctrl.step_height, 0.01F, 0.F, 2.F); });
    changed |= labelled_input("Move Speed",  [&](const char *id) { return ImGui::SliderFloat(id, &ctrl.move_speed, 0.1F, 50.F); });
    changed |= labelled_input("Jump Speed",  [&](const char *id) { return ImGui::SliderFloat(id, &ctrl.jump_speed, 0.1F, 50.F); });
    return changed;
  }

  static auto on_add(dy::SceneRenderer &, dy::Entity e) -> void {
    e.emplace<dy::Components::CharacterController>();
  }
};

template <>
struct ComponentRenderer<dy::Components::Constraint>
    : public BaseComponentRenderer<
          ComponentRenderer<dy::Components::Constraint>> {
  static constexpr std::string_view label = "Constraint";
  static constexpr bool removable = true;
  static constexpr bool addable = true;

  static auto draw(dy::Components::Constraint &constraint, dy::SceneRenderer &,
                   dy::Scene &, dy::Entity &) -> bool {
    bool changed = false;

    static constexpr std::array<const char *, 3> type_names{"Point", "Hinge",
                                                             "Fixed"};
    int type_idx = static_cast<int>(constraint.type);
    ImGui::TextUnformatted("Type");
    if (ImGui::Combo("##constraint_type", &type_idx, type_names.data(),
                     static_cast<int>(type_names.size()))) {
      constraint.type = static_cast<dy::Components::ConstraintType>(type_idx);
      changed = true;
    }

    // Body A/B are entered as raw entity IDs (see the Scene Outliner for an
    // entity's ID); a dedicated entity-picker widget is left for a follow-up.
    changed |= labelled_input("Body A (entity id)", [&](const char *id) {
      int val = static_cast<int>(constraint.body_a);
      const bool edited = ImGui::InputInt(id, &val);
      if (edited)
        constraint.body_a = static_cast<entt::entity>(val);
      return edited;
    });
    changed |= labelled_input("Body B (entity id)", [&](const char *id) {
      int val = static_cast<int>(constraint.body_b);
      const bool edited = ImGui::InputInt(id, &val);
      if (edited)
        constraint.body_b = static_cast<entt::entity>(val);
      return edited;
    });

    changed |= labelled_input("Pivot A", [&](const char *id) { return ImGui::DragFloat3(id, glm::value_ptr(constraint.pivot_a), 0.01F); });
    changed |= labelled_input("Pivot B", [&](const char *id) { return ImGui::DragFloat3(id, glm::value_ptr(constraint.pivot_b), 0.01F); });

    if (constraint.type == dy::Components::ConstraintType::Hinge) {
      changed |= labelled_input("Axis A", [&](const char *id) { return ImGui::DragFloat3(id, glm::value_ptr(constraint.axis_a), 0.01F); });
      changed |= labelled_input("Axis B", [&](const char *id) { return ImGui::DragFloat3(id, glm::value_ptr(constraint.axis_b), 0.01F); });
    }

    return changed;
  }

  static auto on_add(dy::SceneRenderer &, dy::Entity e) -> void {
    e.emplace<dy::Components::Constraint>();
  }
};

inline auto ComponentInspector::draw(dy::SceneRenderer &renderer,
                                     dy::Scene &scene, dy::Entity &entity)
    -> bool {
  std::optional<std::function<void()>> pending_remove;
  std::optional<std::function<void()>> pending_add;

  bool any_change = false;

  dy::for_each_type<dy::MasterComponentList>([&]<typename T>() {
    if constexpr (dy::ComponentConfig<T>::ui_inspectable) {
      static_assert(has_valid_renderer<T>,
                    "Component is marked ui_inspectable but lacks a valid "
                    "ComponentRenderer specialization!");

      any_change |= draw_one<T>(renderer, scene, entity, pending_remove);
    }
  });

  draw_add_button(renderer, entity, pending_add);

  if (pending_remove)
    (*pending_remove)();
  if (pending_add)
    (*pending_add)();

  return any_change;
}

template <typename T>
inline auto ComponentInspector::draw_one(
    dy::SceneRenderer &renderer, dy::Scene &scene, dy::Entity &entity,
    std::optional<std::function<void()>> &pending_remove) -> bool {
  if constexpr (!dy::ComponentConfig<T>::ui_inspectable) {
    return false;
  }

  using R = ComponentRenderer<T>;
  auto *comp = entity.try_get<T>();
  if (comp == nullptr)
    return false;

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

  bool changed = false;
  if (open) {
    ImGui::PushItemWidth(-1.F);
    ImGui::Spacing();
    changed = R::draw(*comp, renderer, scene, entity);
    ImGui::Spacing();
    ImGui::PopItemWidth();
    ImGui::TreePop();
  }

  ImGui::PopID();

  return changed;
}

inline auto ComponentInspector::draw_add_button(
    dy::SceneRenderer &renderer, dy::Entity &entity,
    std::optional<std::function<void()>> &pending_add) -> void {
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
  ImGui::PushStyleColor(ImGuiCol_Button, {0.18F, 0.38F, 0.18F, 1.F});
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.22F, 0.5F, 0.22F, 1.F});
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, {0.28F, 0.62F, 0.28F, 1.F});
  const bool clicked = ImGui::Button("  +  Add Component  ", {-1.F, 0.F});
  ImGui::PopStyleColor(3);

  if (clicked)
    ImGui::OpenPopup("##add_popup");

  if (ImGui::BeginPopup("##add_popup")) {
    dy::for_each_type<dy::MasterComponentList>([&]<typename T>() {
      if constexpr (dy::ComponentConfig<T>::ui_inspectable) {
        using R = ComponentRenderer<T>;
        if constexpr (R::addable) {
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
      }
    });
    ImGui::EndPopup();
  }
}

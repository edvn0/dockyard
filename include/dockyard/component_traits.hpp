#pragma once

#include <cstdint>
#include <dockyard/animation.hpp>
#include <dockyard/binary_stream.hpp>
#include <dockyard/components.hpp>
#include <dockyard/types.hpp>
#include <type_traits>

namespace dy {

template <typename...> constexpr bool always_false = false;

template <typename T> struct ComponentConfig {
  static constexpr bool serializable = true;
  static constexpr bool ui_inspectable = true;
};
template <> struct ComponentConfig<dy::Components::Tag> {
  static constexpr bool serializable = true;
  static constexpr bool ui_inspectable = false;
};
template <> struct ComponentConfig<dy::Components::LocalToWorld> {
  static constexpr bool serializable = true;
  static constexpr bool ui_inspectable = false;
};
template <> struct ComponentConfig<dy::Components::MeshRequest> {
  static constexpr bool serializable = true;
  static constexpr bool ui_inspectable = false;
};
template <> struct ComponentConfig<dy::Components::Mesh> {
  static constexpr bool serializable = true;
  static constexpr bool ui_inspectable = true;
};
template <> struct ComponentConfig<dy::Components::MaterialOverride> {
  static constexpr bool serializable = true;
  static constexpr bool ui_inspectable = true;
};
template <> struct ComponentConfig<dy::Components::DebugFrustum> {
  static constexpr bool serializable = true;
  static constexpr bool ui_inspectable = true;
};
template <> struct ComponentConfig<dy::AnimationState> {
  static constexpr bool serializable = true;
  static constexpr bool ui_inspectable = true;
};

using MasterComponentList =
    std::tuple<dy::Components::Tag, dy::Components::Transform,
               dy::Components::Camera, dy::Components::LocalToWorld,
               dy::Components::MeshRequest, dy::Components::ParentOf,
               dy::Components::Mesh, dy::Components::MaterialOverride,
               dy::Components::DebugFrustum, dy::Components::PointLight,
               dy::AnimationState>;

template <typename Tuple, typename Fn> constexpr void for_each_type(Fn &&fn) {
  []<std::size_t... Is>(auto &&f, std::index_sequence<Is...>) {
    (f.template operator()<std::tuple_element_t<Is, Tuple>>(), ...);
  }(std::forward<Fn>(fn), std::make_index_sequence<std::tuple_size_v<Tuple>>{});
}

template <typename T> struct ComponentSerializer {
  static void save(auto &archive, const T &value)
    requires std::is_trivially_copyable_v<T>
  {
    archive.writer.write(&value, sizeof(T));
  }
  static void load(auto &archive, T &value)
    requires std::is_trivially_copyable_v<T>
  {
    archive.reader.read(&value, sizeof(T));
  }
};

template <> struct ComponentSerializer<Components::Tag> {
  static void save(auto &archive, const Components::Tag &);
  static void load(auto &archive, Components::Tag &);
};

template <> struct ComponentSerializer<Components::MeshRequest> {
  static void save(auto &archive, const Components::MeshRequest &);
  static void load(auto &archive, Components::MeshRequest &);
};

template <> struct ComponentSerializer<Components::Mesh> {
  static void save(auto &archive, const Components::Mesh &);
  static void load(auto &archive, Components::Mesh &);
};

template <> struct ComponentSerializer<Components::Camera> {
  static void save(auto &archive, const Components::Camera &);
  static void load(auto &archive, Components::Camera &);
};

template <> struct ComponentSerializer<Components::Transform> {
  static void save(auto &archive, const Components::Transform &);
  static void load(auto &archive, Components::Transform &);
};

template <> struct ComponentSerializer<Components::LocalToWorld> {
  static void save(auto &archive, const Components::LocalToWorld &);
  static void load(auto &archive, Components::LocalToWorld &);
};

template <> struct ComponentSerializer<Components::PointLight> {
  static void save(auto &archive, const Components::PointLight &);
  static void load(auto &archive, Components::PointLight &);
};

template <> struct ComponentSerializer<AnimationState> {
  static void save(auto &archive, const AnimationState &);
  static void load(auto &archive, AnimationState &);
};

template <> struct ComponentSerializer<Components::MaterialOverride> {
  static void save(auto &archive, const Components::MaterialOverride &);
  static void load(auto &archive, Components::MaterialOverride &);
};

// ---------------------------------------------------------------------------
// Post-load fixup — resolves renderer-owned pointers/handles after deserialization.
//
// Components that hold raw pointers or GPU handles into renderer-owned storage
// (e.g. AnimationState, Components::Mesh) specialize ComponentFixup and set
// needs_fixup = true.  SceneSerializer::post_load_fixup() iterates all such
// components and calls fixup() once assets are available.
// ---------------------------------------------------------------------------

struct FixupContext {
  // Returns {handle, asset*} for a given VFS path; handle/asset may be null/empty if not loaded.
  std::function<std::pair<MeshAssetHandle, const MeshAsset *>(const NullableVFSPath &)> find_mesh;
};

template <typename T> struct ComponentFixup {
  static constexpr bool needs_fixup = false;
};

template <> struct ComponentFixup<Components::Mesh> {
  static constexpr bool needs_fixup = true;
  static void fixup(entt::registry &, entt::entity, Components::Mesh &,
                    const FixupContext &);
};

template <> struct ComponentFixup<AnimationState> {
  static constexpr bool needs_fixup = true;
  static void fixup(entt::registry &, entt::entity, AnimationState &,
                    const FixupContext &);
};

} // namespace dy
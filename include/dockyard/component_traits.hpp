#pragma once

#include <cstdint>
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
  static constexpr bool serializable = false;
  static constexpr bool ui_inspectable = true;
};
template <> struct ComponentConfig<dy::Components::MaterialOverride> {
  static constexpr bool serializable = false;
  static constexpr bool ui_inspectable = true;
};
template <> struct ComponentConfig<dy::Components::DebugFrustum> {
  static constexpr bool serializable = false;
  static constexpr bool ui_inspectable = true;
};

using MasterComponentList =
    std::tuple<dy::Components::Tag, dy::Components::Transform,
               dy::Components::Camera, dy::Components::LocalToWorld,
               dy::Components::MeshRequest, dy::Components::ParentOf,
               dy::Components::Mesh, dy::Components::MaterialOverride,
               dy::Components::DebugFrustum>;

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

} // namespace dy
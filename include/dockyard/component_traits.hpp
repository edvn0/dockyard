#pragma once

#include <cstdint>
#include <dockyard/binary_stream.hpp>
#include <dockyard/components.hpp>
#include <dockyard/types.hpp>
#include <type_traits>

namespace dy {

template <typename T> struct ComponentSerializer {
  static void save(auto &archive, const T &value) {
    static_assert(std::is_trivially_copyable_v<T>,
                  "Component must be trivially copyable, or specialized!");
    archive.writer.write(&value, sizeof(T));
  }
  static void load(auto &archive, T &value) {
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
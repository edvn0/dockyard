#pragma once

#include <cassert>
#include <dockyard/binary_stream.hpp>
#include <dockyard/component_traits.hpp>
#include <dockyard/scene.hpp>
#include <entt/entt.hpp>

namespace dy {

struct CompileTimeOutputArchive {
  BinaryWriter &writer;
  template <typename T> void operator()(const T &val) {
    ComponentSerializer<T>::save(*this, val);
  }
};

struct CompileTimeInputArchive {
  BinaryReader &reader;
  template <typename T> void operator()(T &val) {
    ComponentSerializer<T>::load(*this, val);
  }
};

template <typename... Components> class TemplateSceneSerializer {
public:
  static void serialize(Scene &scene, BinaryWriter &writer) {
    entt::snapshot snapshot{scene.registry()};
    CompileTimeOutputArchive archive{writer};

    snapshot.get<entt::entity>(archive);

    (
        [&]() {
          constexpr u32 type_id = entt::type_hash<Components>::value();
          writer.write(&type_id, sizeof(type_id));
          snapshot.template get<Components>(archive);
        }(),
        ...);
  }

  static void deserialize(Scene &scene, BinaryReader &reader) {
    scene.registry().clear();
    entt::snapshot_loader loader{scene.registry()};
    CompileTimeInputArchive archive{reader};

    loader.get<entt::entity>(archive);

    (
        [&]() {
          u32 expected_id = entt::type_hash<Components>::value();
          u32 read_id = 0;
          reader.read(&read_id, sizeof(read_id));

          assert(read_id == expected_id &&
                 "Scene component layout structural mismatch detected!");
          if (read_id == expected_id) {
            loader.template get<Components>(archive);
          }
        }(),
        ...);
  }
};

using SceneSerializer =
    TemplateSceneSerializer<Components::Tag, Components::Transform,
                            Components::Camera, Components::LocalToWorld,
                            Components::MeshRequest, Components::ParentOf>;

} // namespace dy
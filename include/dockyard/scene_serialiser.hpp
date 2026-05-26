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

template <typename T>
concept has_valid_serializer =
    requires(CompileTimeOutputArchive &out_a, CompileTimeInputArchive &in_a,
             const T &const_val, T &val) {
      { ComponentSerializer<T>::save(out_a, const_val) } -> std::same_as<void>;
      { ComponentSerializer<T>::load(in_a, val) } -> std::same_as<void>;
    };

class SceneSerializer {
public:
  static void serialize(Scene &scene, BinaryWriter &writer) {
    entt::snapshot snapshot{scene.registry()};
    CompileTimeOutputArchive archive{writer};

    snapshot.get<entt::entity>(archive);

    for_each_type<MasterComponentList>([&]<typename Component>() {
      if constexpr (ComponentConfig<Component>::serializable) {

        constexpr bool can_serialize = has_valid_serializer<Component> ||
                                       std::is_trivially_copyable_v<Component>;

        static_assert(
            can_serialize,
            "Component is marked serializable, is NOT trivially copyable, and "
            "lacks a ComponentSerializer specialization!");

        constexpr u32 type_id = entt::type_hash<Component>::value();
        writer.write(&type_id, sizeof(type_id));
        snapshot.template get<Component>(archive);
      }
    });
  }

  static void deserialize(Scene &scene, BinaryReader &reader) {
    scene.registry().clear();
    entt::snapshot_loader loader{scene.registry()};
    CompileTimeInputArchive archive{reader};

    loader.get<entt::entity>(archive);

    for_each_type<MasterComponentList>([&]<typename Component>() {
      if constexpr (ComponentConfig<Component>::serializable) {

        constexpr bool can_serialize = has_valid_serializer<Component> ||
                                       std::is_trivially_copyable_v<Component>;

        static_assert(
            can_serialize,
            "Component is marked serializable, is NOT trivially copyable, and "
            "lacks a ComponentSerializer specialization!");

        u32 expected_id = entt::type_hash<Component>::value();
        u32 read_id = 0;
        reader.read(&read_id, sizeof(read_id));

        assert(read_id == expected_id &&
               "Scene component layout structural mismatch!");
        if (read_id == expected_id) {
          loader.template get<Component>(archive);
        }
      }
    });
  }
};

} // namespace dy
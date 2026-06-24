#pragma once
#include <BS_thread_pool.hpp>
#include <cassert>
#include <dockyard/binary_stream.hpp>
#include <dockyard/component_traits.hpp>
#include <dockyard/scene.hpp>
#include <entt/entt.hpp>
#include <future>
#include <thread>
#include <vector>

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
      if constexpr (ComponentConfig<Component>::serializable &&
                    has_valid_serializer<Component>) {
        constexpr u32 type_id = entt::type_hash<Component>::value();
        writer.write(&type_id, sizeof(type_id));
        snapshot.template get<Component>(archive);
      }
    });
  }

  static void serialize_parallel(BS::priority_thread_pool &thread_pool,
                                 Scene &scene, BinaryWriter &writer) {
    {
      entt::snapshot snapshot{scene.registry()};
      CompileTimeOutputArchive archive{writer};
      snapshot.get<entt::entity>(archive);
    }

    constexpr usize component_count = []() constexpr -> usize {
      usize n = 0;
      for_each_type<MasterComponentList>([&]<typename Component>() {
        if constexpr (ComponentConfig<Component>::serializable &&
                      has_valid_serializer<Component>) {
          ++n;
        }
      });
      return n;
    }();

    struct Slot {
      OwningMemoryWriter writer;
      std::future<void> task;
    };
    std::array<Slot, component_count> slots;

    usize slot_idx = 0;
    for_each_type<MasterComponentList>([&]<typename Component>() {
      if constexpr (ComponentConfig<Component>::serializable &&
                    has_valid_serializer<Component>) {
        const usize my_slot = slot_idx++;

        slots[my_slot].task =
            thread_pool.submit_task([&registry = scene.registry(),
                                     &local_writer = slots[my_slot].writer]() {
              CompileTimeOutputArchive local_archive{local_writer};

              constexpr u32 type_id = entt::type_hash<Component>::value();
              local_writer.write(&type_id, sizeof(type_id));

              entt::snapshot snapshot{registry};
              snapshot.template get<Component>(local_archive);
            });
      }
    });

    for (auto &slot : slots) {
      if (slot.task.valid()) {
        slot.task.get();
        auto buf = std::move(slot.writer.take());
        if (!buf.empty()) {
          writer.write(buf.data(), buf.size());
        }
      }
    }
  }

  static void deserialize(Scene &scene, BinaryReader &reader) {
    entt::snapshot_loader loader{scene.registry()};
    CompileTimeInputArchive archive{reader};
    loader.get<entt::entity>(archive);
    for_each_type<MasterComponentList>([&]<typename Component>() {
      if constexpr (ComponentConfig<Component>::serializable &&
                    has_valid_serializer<Component>) {
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

  // Call after deserialize() once the asset registry is ready.
  // Resolves renderer-owned pointers and handles for all components that
  // declared ComponentFixup<T>::needs_fixup = true.
  static void post_load_fixup(Scene &scene, const FixupContext &ctx) {
    for_each_type<MasterComponentList>([&]<typename Component>() {
      if constexpr (ComponentFixup<Component>::needs_fixup) {
        auto view = scene.registry().view<Component>();
        for (auto [entity, comp] : view.each()) {
          ComponentFixup<Component>::fixup(scene.registry(), entity, comp, ctx);
        }
      }
    });
  }
};

} // namespace dy

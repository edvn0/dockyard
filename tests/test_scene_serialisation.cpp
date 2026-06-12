#include "dockyard/scene_serialiser.hpp"
#include <dockyard/binary_stream.hpp>
#include <dockyard/scene.hpp>
#include <doctest/doctest.h>

static auto thread_pool = BS::priority_thread_pool{};

TEST_CASE("Scene Serialization and Deserialization") {
  using namespace dy;

  Scene original_scene;
  auto ent1 = original_scene.registry().create();
  original_scene.registry().emplace<Components::Tag>(ent1, "MainCameraEntity");

  std::vector<u8> serialization_buffer;

  SUBCASE("Successful Round-trip Serialization") {
    MemoryWriter writer(serialization_buffer);
    REQUIRE_NOTHROW(SceneSerializer::serialize(original_scene, writer));

    Scene deserialized_scene;
    MemoryReader reader(serialization_buffer);
    REQUIRE_NOTHROW(SceneSerializer::deserialize(deserialized_scene, reader));

    auto &dest_registry = deserialized_scene.registry();
    auto view = dest_registry.view<entt::entity>();
    REQUIRE(view.size() == 1);

    auto deserialized_ent = view.front();
    REQUIRE(dest_registry.all_of<Components::Tag>(deserialized_ent));
    CHECK(dest_registry.get<Components::Tag>(deserialized_ent).tag ==
          "MainCameraEntity");
  }
}

TEST_CASE("Scene Serialization with Mesh Components") {
  using namespace dy;

  Scene original_scene;
  auto entity = original_scene.registry().create();

  original_scene.registry().emplace<Components::Tag>(entity,
                                                     "StaticMesh_Object");

  Components::Transform transform{};
  transform.mut().position = glm::vec3(10.0f, -5.5f, 0.2f);
  original_scene.registry().emplace<Components::Transform>(entity, transform);

  Components::Mesh mesh_component{};

  original_scene.registry().emplace<Components::Mesh>(entity, mesh_component);

  std::vector<u8> serialization_buffer;

  {
    MemoryWriter writer(serialization_buffer);
    REQUIRE_NOTHROW(SceneSerializer::serialize(original_scene, writer));
  }

  REQUIRE(serialization_buffer.size() > 0);

  Scene deserialized_scene;

  {
    MemoryReader reader(serialization_buffer);
    REQUIRE_NOTHROW(SceneSerializer::deserialize(deserialized_scene, reader));
  }

  auto &target_registry = deserialized_scene.registry();
  auto view = target_registry.view<entt::entity>();

  REQUIRE(view.size() == 1);
  auto deserialized_entity = view.front();

  REQUIRE(target_registry.all_of<Components::Tag>(deserialized_entity));
  CHECK(target_registry.get<Components::Tag>(deserialized_entity).tag ==
        "StaticMesh_Object");

  REQUIRE(target_registry.all_of<Components::Transform>(deserialized_entity));
  CHECK(target_registry.get<Components::Transform>(deserialized_entity)
            .get()
            .position.y == doctest::Approx(-5.5f));

  REQUIRE(target_registry.all_of<Components::Mesh>(deserialized_entity));
}

#ifdef RUN_SERIALISATION_SMOKE_TESTS
TEST_CASE("Scene Serialization - High Volume Stress Test") {
  using namespace dy;

  constexpr std::string_view str = "Procedural_Static_Mesh_Instance";
  constexpr int entity_count = 1'000'000;

  // -------------------------------------------------------------------------
  // Phase 1: Scene Setup
  // -------------------------------------------------------------------------
  Scene original_scene;
  {
    auto t0 = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < entity_count; ++i) {
      auto entity = original_scene.registry().create();

      original_scene.registry().emplace<Components::Tag>(
          entity, std::format("{}_{}", str, i));

      Components::Transform transform{};
      transform.mut().position =
          glm::vec3(static_cast<float>(i) * 0.1f, 0.0f, -1.0f);
      original_scene.registry().emplace<Components::Transform>(entity,
                                                                transform);

      original_scene.registry().emplace<Components::Mesh>(entity,
                                                           Components::Mesh{});
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    MESSAGE("  [Setup]       ", entity_count, " entities created in ",
            std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0)
                .count(),
            " ms.");
  }

  // -------------------------------------------------------------------------
  // Phase 2: Serialization
  // -------------------------------------------------------------------------
  std::vector<u8> serialization_buffer;
  {
    MemoryWriter writer(serialization_buffer);

    auto t0 = std::chrono::high_resolution_clock::now();
    REQUIRE_NOTHROW(SceneSerializer::serialize(original_scene, writer));
    auto t1 = std::chrono::high_resolution_clock::now();

    const auto duration_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    const auto size_kb = serialization_buffer.size() / 1024;
    const auto throughput_mbps =
        duration_ms > 0
            ? static_cast<double>(serialization_buffer.size()) /
                  (1024.0 * 1024.0) /
                  (static_cast<double>(duration_ms) / 1000.0)
            : 0.0;

    const auto throughput = std::format("{:.1f}", throughput_mbps);
    MESSAGE("  [Serialize]   ", entity_count, " entities -> ", size_kb,
            " KB in ", duration_ms, " ms  (", throughput, " MB/s)");

    REQUIRE(serialization_buffer.size() > 0);
  }

  // -------------------------------------------------------------------------
  // Phase 3: Deserialization
  // -------------------------------------------------------------------------
  Scene deserialized_scene;
  {
    MemoryReader reader(serialization_buffer);

    auto t0 = std::chrono::high_resolution_clock::now();
    REQUIRE_NOTHROW(SceneSerializer::deserialize(deserialized_scene, reader));
    auto t1 = std::chrono::high_resolution_clock::now();

    const auto duration_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    const auto throughput_mbps =
        duration_ms > 0
            ? static_cast<double>(serialization_buffer.size()) /
                  (1024.0 * 1024.0) /
                  (static_cast<double>(duration_ms) / 1000.0)
            : 0.0;

    const auto throughput = std::format("{:.1f}", throughput_mbps);
    MESSAGE("  [Deserialize] ", entity_count, " entities in ", duration_ms,
            " ms  (", throughput, " MB/s)");
  }

  // -------------------------------------------------------------------------
  // Phase 4: Correctness Validation
  // -------------------------------------------------------------------------
  auto &target_registry = deserialized_scene.registry();

  // Entity count integrity
  REQUIRE(target_registry.view<Components::Tag>().size() == entity_count);
  REQUIRE(target_registry.view<Components::Transform>().size() == entity_count);
  REQUIRE(target_registry.view<Components::Mesh>().size() == entity_count);

  // Build a tag -> transform map for O(n) multi-spot validation in one pass
  // rather than three separate O(n) scans.
  struct SpotResult {
    std::string expected_tag;
    float expected_pos_x;
    bool found = false;
    bool position_valid = false;
  };

  const int spots[] = {0, entity_count / 2, entity_count - 1};
  std::array<SpotResult, 3> spot_checks;
  for (int i = 0; i < 3; ++i) {
    spot_checks[i].expected_tag = std::format("{}_{}", str, spots[i]);
    spot_checks[i].expected_pos_x = static_cast<float>(spots[i]) * 0.1f;
  }

  auto view =
      target_registry
          .view<Components::Tag, Components::Transform, Components::Mesh>();

  int remaining = 3;
  for (auto [entity, tag, transform, mesh] : view.each()) {
    if (remaining == 0) break;
    for (auto &spot : spot_checks) {
      if (!spot.found && tag.tag == spot.expected_tag) {
        spot.found = true;
        spot.position_valid =
            std::abs(transform.get().position.x - spot.expected_pos_x) <
            1e-5f;
        --remaining;
        break;
      }
    }
  }

  MESSAGE("  [Spot checks] index=0, index=", entity_count / 2,
          ", index=", entity_count - 1);

  for (int i = 0; i < 3; ++i) {
    const auto &spot = spot_checks[i];
    INFO("Spot check failed for entity index ", spots[i], " (tag='",
         spot.expected_tag, "')");
    REQUIRE(spot.found);
    CHECK(spot.position_valid);
  }
}

TEST_CASE("Scene Serialization - High Volume Stress Test") {
  using namespace dy;

  constexpr std::string_view str = "Procedural_Static_Mesh_Instance";
  constexpr int entity_count = 1'000'000;

  // -------------------------------------------------------------------------
  // Phase 1: Scene Setup
  // -------------------------------------------------------------------------
  Scene original_scene;
  {
    auto t0 = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < entity_count; ++i) {
      auto entity = original_scene.registry().create();

      original_scene.registry().emplace<Components::Tag>(
          entity, std::format("{}_{}", str, i));

      Components::Transform transform{};
      transform.mut().position =
          glm::vec3(static_cast<float>(i) * 0.1f, 0.0f, -1.0f);
      original_scene.registry().emplace<Components::Transform>(entity,
                                                                transform);

      original_scene.registry().emplace<Components::Mesh>(entity,
                                                           Components::Mesh{});
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    MESSAGE("  [Setup]            ",
            entity_count, " entities created in ",
            std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count(),
            " ms.");
  }

  // -------------------------------------------------------------------------
  // Phase 2a: Sequential Serialization
  // -------------------------------------------------------------------------
  std::vector<u8> sequential_buffer;
  {
    MemoryWriter writer(sequential_buffer);

    auto t0 = std::chrono::high_resolution_clock::now();
    REQUIRE_NOTHROW(SceneSerializer::serialize(original_scene, writer));
    auto t1 = std::chrono::high_resolution_clock::now();

    const auto duration_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    const auto throughput =
        std::format("{:.1f}",
            duration_ms > 0
                ? static_cast<double>(sequential_buffer.size()) /
                      (1024.0 * 1024.0) / (static_cast<double>(duration_ms) / 1000.0)
                : 0.0);

    MESSAGE("  [Serialize Seq]    ",
            entity_count, " entities -> ",
            sequential_buffer.size() / 1024, " KB in ",
            duration_ms, " ms  (", throughput, " MB/s)");

    REQUIRE(sequential_buffer.size() > 0);
  }

  // -------------------------------------------------------------------------
  // Phase 2b: Parallel Serialization
  // -------------------------------------------------------------------------
  std::vector<u8> parallel_buffer;
  {
    MemoryWriter writer(parallel_buffer);

    const auto hw = std::thread::hardware_concurrency();
    auto t0 = std::chrono::high_resolution_clock::now();
    REQUIRE_NOTHROW(
        SceneSerializer::serialize_parallel(thread_pool, original_scene, writer));
    auto t1 = std::chrono::high_resolution_clock::now();

    const auto duration_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    const auto throughput =
        std::format("{:.1f}",
            duration_ms > 0
                ? static_cast<double>(parallel_buffer.size()) /
                      (1024.0 * 1024.0) / (static_cast<double>(duration_ms) / 1000.0)
                : 0.0);

    MESSAGE("  [Serialize Par]    ",
            entity_count, " entities -> ",
            parallel_buffer.size() / 1024, " KB in ",
            duration_ms, " ms  (", throughput, " MB/s)  [",
            hw, " logical cores]");

    REQUIRE(parallel_buffer.size() > 0);
  }

  // -------------------------------------------------------------------------
  // Phase 2c: Output equivalence — both paths must produce identical bytes.
  // If this fails the parallel path has an ordering or flushing bug.
  // -------------------------------------------------------------------------
  REQUIRE(sequential_buffer.size() == parallel_buffer.size());
  REQUIRE(sequential_buffer == parallel_buffer);

  // -------------------------------------------------------------------------
  // Phase 3: Deserialization (from sequential buffer — canonical)
  // -------------------------------------------------------------------------
  Scene deserialized_scene;
  {
    MemoryReader reader(sequential_buffer);

    auto t0 = std::chrono::high_resolution_clock::now();
    REQUIRE_NOTHROW(SceneSerializer::deserialize(deserialized_scene, reader));
    auto t1 = std::chrono::high_resolution_clock::now();

    const auto duration_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    const auto throughput =
        std::format("{:.1f}",
            duration_ms > 0
                ? static_cast<double>(sequential_buffer.size()) /
                      (1024.0 * 1024.0) / (static_cast<double>(duration_ms) / 1000.0)
                : 0.0);

    MESSAGE("  [Deserialize]      ",
            entity_count, " entities in ",
            duration_ms, " ms  (", throughput, " MB/s)");
  }

  // -------------------------------------------------------------------------
  // Phase 4: Correctness Validation
  // -------------------------------------------------------------------------
  auto &target_registry = deserialized_scene.registry();

  REQUIRE(target_registry.view<Components::Tag>().size() == entity_count);
  REQUIRE(target_registry.view<Components::Transform>().size() == entity_count);
  REQUIRE(target_registry.view<Components::Mesh>().size() == entity_count);

  // Three anchor spot checks in a single O(n) pass.
  struct SpotResult {
    std::string expected_tag;
    float expected_pos_x;
    bool found = false;
    bool position_valid = false;
  };

  const int spots[] = {0, entity_count / 2, entity_count - 1};
  std::array<SpotResult, 3> spot_checks;
  for (int i = 0; i < 3; ++i) {
    spot_checks[i].expected_tag = std::format("{}_{}", str, spots[i]);
    spot_checks[i].expected_pos_x = static_cast<float>(spots[i]) * 0.1f;
  }

  auto view = target_registry
                  .view<Components::Tag, Components::Transform, Components::Mesh>();

  int remaining = 3;
  for (auto [entity, tag, transform, mesh] : view.each()) {
    if (remaining == 0) break;
    for (auto &spot : spot_checks) {
      if (!spot.found && tag.tag == spot.expected_tag) {
        spot.found = true;
        spot.position_valid =
            std::abs(transform.get().position.x - spot.expected_pos_x) < 1e-5f;
        --remaining;
        break;
      }
    }
  }

  MESSAGE("  [Spot checks]     index=0, index=", entity_count / 2,
          ", index=", entity_count - 1);

  for (int i = 0; i < 3; ++i) {
    const auto &spot = spot_checks[i];
    INFO("Spot check failed for entity index ", spots[i],
         " (tag='", spot.expected_tag, "')");
    REQUIRE(spot.found);
    CHECK(spot.position_valid);
  }
}
#endif

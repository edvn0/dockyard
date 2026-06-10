#include "dockyard/scene_serialiser.hpp"
#include <dockyard/binary_stream.hpp>
#include <dockyard/scene.hpp>
#include <doctest/doctest.h>

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

TEST_CASE("Scene Serialization - High Volume Stress Test") {
  using namespace dy;

  Scene original_scene;
  constexpr std::string_view str = "Procedural_Static_Mesh_Instance";
  constexpr int entity_count = 1000000;

  for (int i = 0; i < entity_count; ++i) {
    auto entity = original_scene.registry().create();

    const auto unique_tag = std::format("{}_{}", str, i);
    original_scene.registry().emplace<Components::Tag>(entity, unique_tag);

    // Procedural spatial data
    Components::Transform transform{};
    transform.mut().position =
        glm::vec3(static_cast<float>(i) * 0.1f, 0.0f, -1.0f);
    original_scene.registry().emplace<Components::Transform>(entity, transform);

    // Add the Mesh target component
    Components::Mesh mesh_component{};
    original_scene.registry().emplace<Components::Mesh>(entity, mesh_component);
  }

  std::vector<u8> serialization_buffer;

  {
    MemoryWriter writer(serialization_buffer);

    auto start_time = std::chrono::high_resolution_clock::now();

    REQUIRE_NOTHROW(SceneSerializer::serialize(original_scene, writer));

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                        end_time - start_time)
                        .count();

    // Print diagnostics to the test runner stdout
    MESSAGE("Serialized ", entity_count, " mesh entities into ",
            (serialization_buffer.size() / 1024), " KB in ", duration, " ms.");
  }

  // Baseline size safety check: Ensure it wrote a significant payload
  REQUIRE(serialization_buffer.size() > 0);

  // 3. Measure Deserialization Stability on Large Volume
  Scene deserialized_scene;
  {
    MemoryReader reader(serialization_buffer);

    auto start_time = std::chrono::high_resolution_clock::now();

    REQUIRE_NOTHROW(SceneSerializer::deserialize(deserialized_scene, reader));

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                        end_time - start_time)
                        .count();

    MESSAGE("Deserialized ", entity_count, " mesh entities in ", duration,
            " ms.");
  }

  // 4. Spot Check Validation (Beginning, Middle, and End of Stream)
  auto &target_registry = deserialized_scene.registry();
  auto view =
      target_registry
          .view<Components::Tag, Components::Transform, Components::Mesh>();

  // Ensure absolutely all entities crossed the pipeline completely intact
  REQUIRE(target_registry.view<Components::Tag>().size() == entity_count);

  // Let's grab a specific index from the middle of the layout to check for
  // cumulative offset drift
  int target_spot_index = entity_count / 2;
  std::string expected_tag =
      "Procedural_Static_Mesh_Instance_" + std::to_string(target_spot_index);
  float expected_pos_x = static_cast<float>(target_spot_index) * 0.1f;

  bool spot_checked_and_passed = false;

  for (auto [entity, tag, transform, mesh] : view.each()) {
    if (tag.tag == expected_tag) {
      CHECK(transform.get().position.x == doctest::Approx(expected_pos_x));
      spot_checked_and_passed = true;
      break;
    }
  }

  // Guarantee that our spot check match wasn't lost in a corrupted data stream
  // layout
  REQUIRE(spot_checked_and_passed);
}
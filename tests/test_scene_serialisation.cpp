#include "dockyard/scene_serialiser.hpp"
#include <dockyard/animation.hpp>
#include <dockyard/binary_stream.hpp>
#include <dockyard/mesh.hpp>
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

TEST_CASE("Post-load fixup") {
  using namespace dy;

  // Shared asset: one skeleton (root joint) and two named clips.
  MeshAsset fake_asset;
  {
    Skeleton skel;
    SkeletonJoint joint;
    joint.parent = -1;
    joint.node_index = 0;
    joint.name = "root";
    joint.bind_translation = glm::vec3{0.F};
    joint.bind_rotation = glm::quat{1.F, 0.F, 0.F, 0.F};
    joint.bind_scale = glm::vec3{1.F};
    joint.inverse_bind = glm::mat4{1.F};
    skel.joints.push_back(joint);
    fake_asset.skeletons.push_back(std::move(skel));

    AnimationClip walk;
    walk.name = "Walk";
    walk.duration = 2.0F;
    fake_asset.animations.push_back(std::move(walk));

    AnimationClip run;
    run.name = "Run";
    run.duration = 1.0F;
    fake_asset.animations.push_back(std::move(run));
  }

  const NullableVFSPath mesh_path =
      NullableVFSPath::create("meshes://character.glb");
  const MeshAssetHandle test_handle{1, 1};

  auto make_context = [&]() -> FixupContext {
    return FixupContext{
        .find_mesh = [&](const NullableVFSPath &path)
            -> std::pair<MeshAssetHandle, const MeshAsset *> {
          if (path.valid() && path.view() == mesh_path.view())
            return {test_handle, &fake_asset};
          return {{}, nullptr};
        }};
  };

  SUBCASE(
      "Given_AnimationState_When_RoundTrip_Then_ScalarFieldsSurviveAndPointersAreNull") {
    // Given: entity with Mesh + AnimationState playing clip "Walk" at t=1.5
    Scene original;
    auto ent = original.registry().create();
    original.registry().emplace<Components::Mesh>(
        ent, Components::Mesh{.handle = {}, .source_path = mesh_path});
    AnimationState anim = AnimationState::create(
        &fake_asset.skeletons[0], &fake_asset.animations[0], 0);
    anim.time = 1.5F;
    anim.loop = false;
    original.registry().emplace<AnimationState>(ent, std::move(anim));

    // When: serialize then deserialize (no fixup yet)
    std::vector<u8> buf;
    {
      MemoryWriter writer{buf};
      SceneSerializer::serialize(original, writer);
    }
    Scene loaded;
    {
      MemoryReader reader{buf};
      SceneSerializer::deserialize(loaded, reader);
    }

    // Then: scalar fields preserved, pointers null
    auto view = loaded.registry().view<AnimationState>();
    REQUIRE(view.size() == 1);
    const auto &result = view.get<AnimationState>(view.front());
    CHECK(result.pending_clip_name == "Walk");
    CHECK(result.time == doctest::Approx(1.5F));
    CHECK(result.loop == false);
    CHECK(result.skeleton_index == 0);
    CHECK(result.skeleton == nullptr);
    CHECK(result.clip == nullptr);
  }

  SUBCASE(
      "Given_AnimationStateAfterDeserialize_When_PostLoadFixup_Then_PointersResolvedByName") {
    // Given: deserialized scene with "Run" AnimationState
    Scene original;
    auto ent = original.registry().create();
    original.registry().emplace<Components::Mesh>(
        ent, Components::Mesh{.handle = {}, .source_path = mesh_path});
    AnimationState anim = AnimationState::create(
        &fake_asset.skeletons[0], &fake_asset.animations[1], 0); // "Run"
    anim.time = 0.5F;
    anim.loop = true;
    original.registry().emplace<AnimationState>(ent, std::move(anim));

    std::vector<u8> buf;
    {
      MemoryWriter writer{buf};
      SceneSerializer::serialize(original, writer);
    }
    Scene loaded;
    {
      MemoryReader reader{buf};
      SceneSerializer::deserialize(loaded, reader);
    }

    // When
    SceneSerializer::post_load_fixup(loaded, make_context());

    // Then: pointers point into fake_asset, scalars preserved
    auto view = loaded.registry().view<AnimationState>();
    REQUIRE(view.size() == 1);
    const auto &result = view.get<AnimationState>(view.front());
    CHECK(result.skeleton == &fake_asset.skeletons[0]);
    REQUIRE(result.clip != nullptr);
    CHECK(result.clip->name == "Run");
    CHECK(result.time == doctest::Approx(0.5F));
    CHECK(result.loop == true);
  }

  SUBCASE(
      "Given_UnknownClipName_When_PostLoadFixup_Then_FallsBackToFirstClip") {
    // Given: entity whose pending_clip_name doesn't exist in the asset
    Scene scene;
    auto ent = scene.registry().create();
    scene.registry().emplace<Components::Mesh>(
        ent, Components::Mesh{.handle = {}, .source_path = mesh_path});
    AnimationState anim{};
    anim.pending_clip_name = "Nonexistent";
    anim.skeleton_index = 0;
    scene.registry().emplace<AnimationState>(ent, std::move(anim));

    // When
    SceneSerializer::post_load_fixup(scene, make_context());

    // Then: falls back to animations[0] ("Walk")
    const auto &result = scene.registry().get<AnimationState>(ent);
    REQUIRE(result.clip != nullptr);
    CHECK(result.clip == &fake_asset.animations[0]);
    CHECK(result.clip->name == "Walk");
  }

  SUBCASE(
      "Given_AnimationStateWithNoMeshComponent_When_PostLoadFixup_Then_SkippedGracefully") {
    // Given: entity with AnimationState but no Mesh component
    Scene scene;
    auto ent = scene.registry().create();
    AnimationState anim{};
    anim.pending_clip_name = "Walk";
    anim.skeleton_index = 0;
    scene.registry().emplace<AnimationState>(ent, std::move(anim));

    // When / Then: no crash, pointers remain null
    REQUIRE_NOTHROW(SceneSerializer::post_load_fixup(scene, make_context()));
    const auto &result = scene.registry().get<AnimationState>(ent);
    CHECK(result.skeleton == nullptr);
    CHECK(result.clip == nullptr);
  }

  SUBCASE("Given_MeshWithPath_When_PostLoadFixup_Then_HandleIsSet") {
    // Given: Mesh component with source_path set but empty handle
    Scene scene;
    auto ent = scene.registry().create();
    scene.registry().emplace<Components::Mesh>(
        ent, Components::Mesh{.handle = {}, .source_path = mesh_path});

    // When
    SceneSerializer::post_load_fixup(scene, make_context());

    // Then: handle matches what the context returned
    const auto &mesh = scene.registry().get<Components::Mesh>(ent);
    CHECK(mesh.handle == test_handle);
  }

  SUBCASE(
      "Given_MeshWithUnknownPath_When_PostLoadFixup_Then_HandleRemainsEmpty") {
    // Given: Mesh with a path the context doesn't recognise
    Scene scene;
    auto ent = scene.registry().create();
    scene.registry().emplace<Components::Mesh>(
        ent, Components::Mesh{.handle = {},
                              .source_path = NullableVFSPath::create(
                                  "meshes://unknown.glb")});

    // When / Then: no crash, handle stays empty
    REQUIRE_NOTHROW(SceneSerializer::post_load_fixup(scene, make_context()));
    const auto &mesh = scene.registry().get<Components::Mesh>(ent);
    CHECK(mesh.handle.empty());
  }
}

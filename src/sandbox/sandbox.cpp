#include <cmath>
#include <dockyard/asset_loader.hpp>
#include <dockyard/components.hpp>
#include <dockyard/game_memory.hpp>
#include <dockyard/igame.hpp>
#include <dockyard/log.hpp>
#include <dockyard/scene.hpp>
#include <glm/gtc/random.hpp>
#include <numbers>
#include <vector>

#include <tracy/Tracy.hpp>

#include <glm/gtc/type_ptr.hpp>

using namespace dy;

struct HelmetTag {};

struct HelmetData {
  entt::entity entity;
  glm::vec3 origin;
  float phase;
};

struct SandboxState {
  float time = 0.0f;
  entt::entity player = entt::null;
  std::vector<HelmetData> helmets;
};

struct Sandbox : IGame {
  MeshAssetHandle helmet_mesh{};

  void pre_init(IAssetLoader &assets) override {
    ZoneScopedNC("Sandbox::pre_init", 0x00FF88);
    auto result = assets.load_mesh(
        VFSPath::create("meshes://damaged_helmet/DamagedHelmet.glb"));
    if (result) {
      helmet_mesh = *result;
      dy::info("Sandbox: helmet mesh ready (handle index {})",
               helmet_mesh.index());
    } else {
      dy::warn("Sandbox: failed to preload helmet mesh: {}", result.error());
    }
  }

  auto load_scene(Scene &scene, IAssetLoader &assets) {
    auto make_wall = [&](std::string_view name, glm::vec3 pos, glm::vec3 scl) {
      auto e = scene.make(name);
      auto &mc = e.emplace<Components::Mesh>();
      mc.handle = assets.load_mesh(VFSPath::create("engine://cube")).value();
      mc.source_path = VFSPath::create("engine://cube");
      auto t = e.get<Components::Transform>().mut();
      t.position = pos;
      t.scale = scl;

      auto &colour = e.emplace_or_replace<Components::MaterialOverride>();
      colour.dirty = true;
      const auto wall_colour = glm::linearRand(
          glm::vec4{0.0F, 0.0F, 0.0F, 1.0F}, glm::vec4{1.0F, 1.0F, 1.0F, 1.0F});
      std::ranges::copy_n(glm::value_ptr(wall_colour), 4,
                          colour.material.albedo_factor);
    };

    constexpr float human_scale_y = 1.75F / 7.826F;
    constexpr float human_foot_y = 3.913F * human_scale_y;

    auto make_human = [&](std::string_view name, float x, float z) {
      auto e = scene.make(name);
      auto &mc = e.emplace<Components::Mesh>();
      mc.handle = assets.load_mesh(VFSPath::create("engine://capsule")).value();
      mc.source_path = VFSPath::create("engine://capsule");
      auto t = e.get<Components::Transform>().mut();
      t.position = {x, human_foot_y, z};
      t.scale = {0.2F, human_scale_y, 0.2F};
    };

    constexpr float wh = 4.0F; // wall height
    constexpr float wt = 0.5F; // wall thickness
    constexpr float wy =
        wh * 0.5F; // wall center Y (bottom sits on floor at y=0)

    // Floor — surface at y=0
    make_wall("Floor", {0.0F, -0.5F, 0.0F}, {60.0F, 1.0F, 60.0F});

    // Central room (x: -8..8, z: -8..8)
    // South wall — door gap at center (x: -1.5..1.5)
    make_wall("Wall_Central_S_L", {-4.75F, wy, -8.0F}, {6.5F, wh, wt});
    make_wall("Wall_Central_S_R", {4.75F, wy, -8.0F}, {6.5F, wh, wt});
    // North wall — door gap for north corridor (x: -2..2)
    make_wall("Wall_Central_N_L", {-5.0F, wy, 8.0F}, {6.0F, wh, wt});
    make_wall("Wall_Central_N_R", {5.0F, wy, 8.0F}, {6.0F, wh, wt});
    // West wall — solid
    make_wall("Wall_Central_W", {-8.0F, wy, 0.0F}, {wt, wh, 16.0F});
    // East wall — door gap for east wing (z: -5..5)
    make_wall("Wall_Central_E_N", {8.0F, wy, 6.5F}, {wt, wh, 3.0F});
    make_wall("Wall_Central_E_S", {8.0F, wy, -6.5F}, {wt, wh, 3.0F});

    // Central room columns
    make_wall("Column_NE", {4.0F, wy, 4.0F}, {0.8F, wh, 0.8F});
    make_wall("Column_NW", {-4.0F, wy, 4.0F}, {0.8F, wh, 0.8F});
    make_wall("Column_SE", {4.0F, wy, -4.0F}, {0.8F, wh, 0.8F});
    make_wall("Column_SW", {-4.0F, wy, -4.0F}, {0.8F, wh, 0.8F});

    // North corridor (x: -2..2, z: 8..20)
    make_wall("Wall_CorridorN_W", {-2.0F, wy, 14.0F}, {wt, wh, 12.0F});
    make_wall("Wall_CorridorN_E", {2.0F, wy, 14.0F}, {wt, wh, 12.0F});
    make_wall("Wall_CorridorN_End", {0.0F, wy, 20.0F}, {4.0F, wh, wt});

    // South room (x: -10..10, z: -8..-20)
    // Filler pieces connecting wider south room to central room south wall
    make_wall("Wall_South_ExtW", {-9.0F, wy, -8.0F}, {2.0F, wh, wt});
    make_wall("Wall_South_ExtE", {9.0F, wy, -8.0F}, {2.0F, wh, wt});
    make_wall("Wall_South_W", {-10.0F, wy, -14.0F}, {wt, wh, 12.0F});
    make_wall("Wall_South_E", {10.0F, wy, -14.0F}, {wt, wh, 12.0F});
    make_wall("Wall_South_End", {0.0F, wy, -20.0F}, {20.0F, wh, wt});
    // Interior divider with a 2-wide gap at center
    make_wall("Wall_South_Div_L", {-5.5F, wy, -14.0F}, {9.0F, wh, wt});
    make_wall("Wall_South_Div_R", {5.5F, wy, -14.0F}, {9.0F, wh, wt});

    // East wing (x: 8..20, z: -5..5)
    make_wall("Wall_East_N", {14.0F, wy, 5.0F}, {12.0F, wh, wt});
    make_wall("Wall_East_S", {14.0F, wy, -5.0F}, {12.0F, wh, wt});
    make_wall("Wall_East_End", {20.0F, wy, 0.0F}, {wt, wh, 10.0F});
    make_wall("Column_East", {14.0F, wy, 0.0F}, {0.8F, wh, 0.8F});

    // Humans
    make_human("Human_01", -6.0F, 1.0F);   // central room, near west wall
    make_human("Human_02", 6.0F, -1.0F);   // central room, near east wall
    make_human("Human_03", 0.0F, -6.0F);   // central room, near south door
    make_human("Human_04", 3.5F, 3.5F);    // near NE column
    make_human("Human_05", -3.5F, -3.5F);  // near SW column
    make_human("Human_06", 0.5F, 0.0F);    // central room, open space
    make_human("Human_07", 0.0F, 10.0F);   // north corridor, entrance
    make_human("Human_08", -0.5F, 14.5F);  // north corridor, mid
    make_human("Human_09", 0.5F, 18.5F);   // north corridor, near end
    make_human("Human_10", -6.0F, -10.0F); // south room, left side
    make_human("Human_11", 6.0F, -11.0F);  // south room, right side
    make_human("Human_12", -1.5F, -12.5F); // south room, near divider
    make_human("Human_13", 3.0F, -17.0F);  // south room, far end
    make_human("Human_14", 10.5F, 2.0F);   // east wing, near entrance
    make_human("Human_15", 17.0F, -1.0F);  // east wing, far end

    // Lights — one per zone, positioned near ceiling
    auto light_parent = scene.make("Lights");
    auto make_light = [&](float x, float z, float intensity, float radius) {
      auto light = scene.make("Light", light_parent);
      auto &l = light.emplace<Components::PointLight>();
      l.color = glm::linearRand(glm::vec3{0.8F}, glm::vec3{1.F});
      l.intensity = intensity;
      l.radius = radius;
      light.get<Components::Transform>().mut().position = {x, 3.5F, z};
    };

    make_light(0.0F, 0.0F, 3.0F, 14.0F);   // central room, center
    make_light(5.0F, 5.0F, 2.0F, 8.0F);    // central room, NE corner
    make_light(-5.0F, -5.0F, 2.0F, 8.0F);  // central room, SW corner
    make_light(0.0F, 11.0F, 2.0F, 6.0F);   // north corridor, near entrance
    make_light(0.0F, 18.0F, 2.0F, 6.0F);   // north corridor, far end
    make_light(0.0F, -10.0F, 2.5F, 12.0F); // south room, front half
    make_light(-4.0F, -17.0F, 2.0F, 8.0F); // south room, back left
    make_light(4.0F, -17.0F, 2.0F, 8.0F);  // south room, back right
    make_light(12.0F, 0.0F, 2.5F, 8.0F);   // east wing, center
    make_light(18.5F, 0.0F, 2.0F, 5.0F);   // east wing, far end

    assets.notify_material_overrides_added();
  }

  void init(GameMemory *mem, Scene *scene,
            IAssetLoader &asset_loader) override {
    ZoneScopedNC("Sandbox::init", 0x00FF88);

    load_scene(*scene, asset_loader);

    auto *state = mem->allocate<SandboxState>();

    auto entity = scene->make("Player 4");
    state->player = entity.id();

    if (helmet_mesh.valid()) {
      constexpr auto size = 25.0F;
      auto parent = scene->make("Helmet parent");
      for (auto i = 0; i < 300; i++) {
        auto child = scene->make("Helmet", parent);

        child.emplace<Components::Mesh>(
            helmet_mesh,
            VFSPath::create("meshes://damaged_helmet/DamagedHelmet.glb"));
        child.get<Components::Transform>().mut().position =
            glm::linearRand(glm::vec3{-size}, glm::vec3{size});
      }
    }

    using namespace Components;
    scene->view<Transform, Components::Mesh>().each(
        [&](entt::entity e, const Transform &transform,
            const Components::Mesh &mesh) {
          if (!mesh.handle.valid())
            return;

          scene->registry().emplace<HelmetTag>(e);
          state->helmets.push_back({
              .entity = e,
              .origin = transform.get().position,
              .phase = 0.0f,
          });
        });

    const float tau = 2.0f * std::numbers::pi_v<float>;
    const auto count = static_cast<float>(state->helmets.size());
    for (int i = 0; i < static_cast<int>(state->helmets.size()); ++i)
      state->helmets[i].phase = static_cast<float>(i) / count * tau;
  }

  void update(GameMemory *mem, Scene *scene, float ts) override {
    ZoneScopedNC("Sandbox::update", 0x00FF88);
    auto *state = mem->root<SandboxState>();
    state->time += ts;

    using namespace Components;

    const float radius = 5.0f;
    const float speed = 1.F;

    for (const auto &hd : state->helmets) {
      auto &transform = scene->registry().get<Transform>(hd.entity);
      const float angle = state->time * speed + hd.phase;

      auto t = transform.mut();
      t.position.x = hd.origin.x + std::cos(angle) * radius;
      t.position.z = hd.origin.z + std::sin(angle) * radius;
    }
  }

  void destroy(GameMemory *mem, Scene *scene) override {
    ZoneScopedNC("Sandbox::destroy", 0x00FF88);
    auto *state = mem->root<SandboxState>();
    if (scene->registry().valid(state->player))
      scene->registry().destroy(state->player);
  }
};

GAME_EXPORT dy::IGame *create_game() { return new Sandbox{}; }

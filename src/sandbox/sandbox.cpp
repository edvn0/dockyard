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

  void init(GameMemory *mem, Scene *scene, IAssetLoader &) override {
    ZoneScopedNC("Sandbox::init", 0x00FF88);
    auto *state = mem->allocate<SandboxState>();

    auto entity = scene->make("Player 4");
    state->player = entity.id();

    if (helmet_mesh.valid()) {
      constexpr auto size = 25.0F;
      auto parent = scene->make("Helmet parent");
      for (auto i = 0; i < 300; i++) {
        auto child = scene->make("Helmet", parent);

        child.emplace<Components::Mesh>(helmet_mesh);
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

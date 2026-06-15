#include <cmath>
#include <dockyard/components.hpp>
#include <dockyard/game_memory.hpp>
#include <dockyard/igame.hpp>
#include <dockyard/scene.hpp>
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
  void init(GameMemory *mem, Scene *scene) override {
    ZoneScopedNC("Sandbox::init", 0x00FF88);
    auto *state = mem->allocate<SandboxState>();

    auto entity = scene->make("Player 4");
    state->player = entity.id();

    using namespace Components;

    scene->view<Transform, Components::Mesh>().each(
        [&](entt::entity e, const Transform &transform,
            const Components::Mesh &mesh) {
          if (mesh.handle.index() != 1)
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
    const float speed = 20.F;

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
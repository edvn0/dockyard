#include <dockyard/components.hpp>
#include <dockyard/game_memory.hpp>
#include <dockyard/igame.hpp>
#include <dockyard/scene.hpp>

using namespace dy;

struct SandboxState {
    float        time   = 0.0f;
    entt::entity player = entt::null;
};

struct Sandbox : IGame {
    void init(GameMemory* mem, Scene* scene) override {
        auto* state  = mem->allocate<SandboxState>();
        auto  entity = scene->make("Player");
        entity.emplace<Components::Transform>();
        entity.emplace<Components::LocalToWorld>();
        state->player = entity.id();
    }

    void update(GameMemory* mem, Scene* scene, float ts) override {
        auto* state = mem->root<SandboxState>();
        state->time += ts;
    }

    void destroy(GameMemory* mem, Scene* scene) override {
        auto* state = mem->root<SandboxState>();
        if (scene->registry().valid(state->player))
            scene->registry().destroy(state->player);
    }
};

GAME_EXPORT dy::IGame* create_game() {
    return new Sandbox{};
}

#include <dockforge/dockforge.hpp>

#include <dockforge/editor_camera.hpp>

#include <dockyard/animation.hpp>
#include <dockyard/components.hpp>
#include <dockyard/scene_renderer.hpp>

using namespace dy;

static auto update_local_to_world_matrices(entt::registry &registry) -> void {
  ZoneScopedNC("update_local_to_world_matrices", 0x98FB98);
  auto render_group =
      registry.group<Components::Transform, Components::LocalToWorld,
                     Components::Mesh>();

  for (auto e : render_group) {
    auto &transform = render_group.get<Components::Transform>(e);

    if (!transform.dirty())
      continue;

    auto &ltw = render_group.get<Components::LocalToWorld>(e);

    auto &&[position, rotation, scale] = transform.get();

    ltw.matrix = glm::translate(glm::mat4{1.0F}, position) *
                 glm::mat4_cast(rotation) * glm::scale(glm::mat4{1.0F}, scale);

    transform.set_dirty(false);
  }
}

auto Dockforge::update(float ts) -> void {
  ZoneScopedNC("Dockforge::update", 0x00BFFF);
  if (!sim_state.in<sim::S::Editing>() && script_engine &&
      script_engine->loaded()) {
    if (script_engine->poll_reload()) {
      script_engine->end_play(active_scene);
      script_engine->begin_play(active_scene);
      renderer->override_pool.needs_grow = true;
      info("Lua script hot reloaded");
      TracyMessage("Lua hot reloaded", 16);
    }
    if (sim_state.in<sim::S::Playing>())
      script_engine->tick(active_scene, ts);
  }
  if (active_scene->primary_camera() == nullptr)
    editor_camera->update(ts);
  {
    // Don't advance animations
    const auto timestep = sim_state.in<sim::S::Playing>() ? ts : 0.0F;
    animation_state_update(timestep);
  }

  update_local_to_world_matrices(active_scene->registry());
}

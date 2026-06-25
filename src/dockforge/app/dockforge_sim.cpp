#include <dockforge/dockforge.hpp>

#include <dockyard/animation.hpp>
#include <dockyard/asset_loader.hpp>
#include <dockyard/binary_stream.hpp>
#include <dockyard/components.hpp>
#include <dockyard/scene_renderer.hpp>
#include <dockyard/scene_serialiser.hpp>

using namespace dy;

static constexpr float step_dt = 1.0F / 60.0F;

auto Dockforge::pause() -> void {
  sim_state.try_transition<sim::S::Paused>(
      [&](auto) { TracyMessage("Game paused", 11); });
}

auto Dockforge::resume() -> void {
  sim_state.try_transition<sim::S::Playing>(
      [&](auto) { TracyMessage("Game resumed", 12); });
}

auto Dockforge::step() -> void {
  if (!sim_state.in<sim::S::Paused>() || !script_engine ||
      !script_engine->loaded())
    return;
  script_engine->update(active_scene, step_dt);
  animation_state_update(step_dt);
}

auto Dockforge::play() -> void {
  if (!script_engine || !script_engine->loaded())
    return;

  sim_state.try_transition<sim::S::Playing>([&](auto) {
    runtime_scene = std::make_shared<Scene>();

    std::vector<u8> snapshot_buf;
    MemoryWriter writer{snapshot_buf};
    SceneSerializer::serialize_parallel(renderer->thread_pool, *editor_scene,
                                        writer);
    MemoryReader reader{snapshot_buf};
    SceneSerializer::deserialize(*runtime_scene, reader);

    FixupContext fixup_ctx{
        .find_mesh = [&](const NullableVFSPath &path)
            -> std::pair<MeshAssetHandle, const MeshAsset *> {
          if (!path.valid())
            return {{}, nullptr};
          auto result = asset_loader->load_mesh(path.value());
          if (!result) {
            warn("Failed to resolve mesh '{}': {}", path.view(),
                 result.error());
            return {{}, nullptr};
          }
          MeshAssetHandle handle = *result;
          return {handle, renderer->get_mesh(handle)};
        }};
    SceneSerializer::post_load_fixup(*runtime_scene, fixup_ctx);

    runtime_scene->group<Components::Transform, Components::LocalToWorld,
                         Components::Mesh>();

    active_scene = runtime_scene.get();
    editor_state.active_scene = active_scene;
    editor_state.selected = entt::null;
    editor_state.hierarchy_dirty = true;
    script_engine->init(active_scene, *asset_loader);
    TracyMessage("Game started", 12);
    editor_state.cache_dirty = true;
  });
}

auto Dockforge::stop() -> void {
  sim_state.try_transition<sim::S::Editing>([&](auto) {
    if (script_engine && script_engine->loaded())
      script_engine->destroy(active_scene);

    // Free every runtime-allocated override slot back into the pool.
    if (runtime_scene) {
      auto view =
          runtime_scene->registry().view<Components::MaterialOverride>();
      for (auto &&[e, ov] : view.each()) {
        if (ov.gpu_slot != Components::MaterialOverride::invalid_material)
          renderer->override_pool.free(ov.gpu_slot);
      }
    }

    // Drop all free-list state and reset the sequential counter. The pool may
    // have grown and relocated its base during play; editor entities could hold
    // stale slot indices. Resetting to zero lets flush_material_overrides()
    // re-allocate clean slots from the current pool base on the first
    // post-stop frame.
    renderer->override_pool.next = 0;
    renderer->override_pool.free_slots.clear();
    renderer->override_pool.needs_grow = false;

    // Invalidate editor entity slots so they are re-acquired next frame.
    {
      auto view = editor_scene->registry().view<Components::MaterialOverride>();
      for (auto &&[e, ov] : view.each()) {
        ov.gpu_slot = Components::MaterialOverride::invalid_material;
        ov.dirty = true;
      }
    }

    TracyMessage("Game stopped", 12);
    active_scene = editor_scene.get();
    editor_state.active_scene = active_scene;
    runtime_scene.reset();
    editor_state.selected = entt::null;
    editor_state.cache_dirty = true;
    for (auto &panel : panels)
      panel->on_stop();
  });

  // TODO: This should maybe not be necessary, but whatever.
  vkDeviceWaitIdle(context->device);
}

void Dockforge::animation_state_update(float ts) {
  ZoneScopedNC("Animation state update", 0xFF6611);
  PROFILE_SCOPE("Animation State");
  if (ts <= 0.0F)
    return;

  const auto view = active_scene->view<AnimationState>();
  std::vector<std::function<void()>> funcs{};
  funcs.reserve(view.size());

  for (auto &&[entity, animation_state] : view.each()) {
    funcs.emplace_back([t = ts, &anim = animation_state] { anim.advance(t); });
  }

  renderer->thread_pool.submit_bulk(funcs).wait();
}

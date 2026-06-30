#include <dockforge/dockforge.hpp>

#include <dockforge/editor_camera.hpp>

#include <dockyard/animation.hpp>
#include <dockyard/asset_loader.hpp>
#include <dockyard/binary_stream.hpp>
#include <dockyard/components.hpp>
#include <dockyard/scene_renderer.hpp>
#include <dockyard/scene_serialiser.hpp>

#include <GLFW/glfw3.h>

using namespace dy;

static constexpr float step_dt = 1.0F / 60.0F;

auto Dockforge::pause() -> void {
  sim_state.try_transition<sim::S::Paused>([&](auto) {
    glfwSetInputMode(App::get_window(), GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    TracyMessage("Game paused", 11);
  });
}

auto Dockforge::resume() -> void {
  sim_state.try_transition<sim::S::Playing>([&](auto) {
    if (!active_scene->view<Components::FirstPersonController>().empty())
      glfwSetInputMode(App::get_window(), GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    TracyMessage("Game resumed", 12);
  });
}

auto Dockforge::step() -> void {
  if (!sim_state.in<sim::S::Paused>() || !script_engine ||
      !script_engine->loaded())
    return;
  script_engine->tick(active_scene, step_dt);
  animation_state_update(step_dt);
}

auto Dockforge::play() -> void {
  if (!script_engine || !script_engine->loaded())
    return;

  sim_state.try_transition<sim::S::Playing>([&](auto) {
    info("[Sim] play(): override_pool next={} cap={} free={} base_slot={}",
         renderer->override_pool.next, renderer->override_pool.capacity,
         renderer->override_pool.free_slots.size(),
         renderer->override_pool.base_slot);

    runtime_scene = std::make_shared<Scene>();

    std::vector<u8> snapshot_buf;
    MemoryWriter writer{snapshot_buf};
    SceneSerializer::serialize_parallel(renderer->thread_pool, *editor_scene,
                                        writer);
    info("[Sim] play(): serialized {} bytes", snapshot_buf.size());
    MemoryReader reader{snapshot_buf};
    SceneSerializer::deserialize(*runtime_scene, reader);
    info("[Sim] play(): deserialized runtime scene");

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
    info("[Sim] play(): post_load_fixup done, flat_prim_table size={}",
         renderer->flat_prim_table.size());

    runtime_scene->group<Components::Transform, Components::LocalToWorld,
                         Components::Mesh>();

    active_scene = runtime_scene.get();
    editor_state.active_scene = active_scene;
    editor_state.selected = entt::null;
    editor_state.hierarchy_dirty = true;

    auto cam_entity = runtime_scene->make("Player Camera");
    cam_entity.emplace<Components::Camera>(editor_camera->camera);
    cam_entity.emplace<Components::FirstPersonController>();
    runtime_scene->set_primary_camera(cam_entity.handle());
    glfwSetInputMode(App::get_window(), GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    info("[Sim] play(): calling begin_play");
    script_engine->begin_play(active_scene);
    TracyMessage("Game started", 12);
    info("[Sim] play(): complete");
    editor_state.cache_dirty = true;
  });
}

auto Dockforge::stop() -> void {
  glfwSetInputMode(App::get_window(), GLFW_CURSOR, GLFW_CURSOR_NORMAL);

  sim_state.try_transition<sim::S::Editing>([&](auto) {
    info("[Sim] stop(): override_pool next={} cap={} free={} base_slot={}",
         renderer->override_pool.next, renderer->override_pool.capacity,
         renderer->override_pool.free_slots.size(),
         renderer->override_pool.base_slot);

    if (script_engine && script_engine->loaded()) {
      info("[Sim] stop(): calling end_play");
      script_engine->end_play(active_scene);
    }

    // Free every runtime-allocated override slot back into the pool.
    u32 freed_runtime = 0;
    if (runtime_scene) {
      auto view =
          runtime_scene->registry().view<Components::MaterialOverride>();
      for (auto &&[e, ov] : view.each()) {
        if (ov.gpu_slot != Components::MaterialOverride::invalid_material) {
          renderer->override_pool.free(ov.gpu_slot);
          ++freed_runtime;
        }
      }
    }
    info("[Sim] stop(): freed {} runtime override slots", freed_runtime);

    // Drop all free-list state and reset the sequential counter. The pool may
    // have grown and relocated its base during play; editor entities could hold
    // stale slot indices. Resetting to zero lets flush_material_overrides()
    // re-allocate clean slots from the current pool base on the first
    // post-stop frame.
    renderer->override_pool.next = 0;
    renderer->override_pool.free_slots.clear();
    renderer->override_pool.needs_grow = false;

    // Invalidate editor entity slots so they are re-acquired next frame.
    u32 invalidated = 0;
    {
      auto view = editor_scene->registry().view<Components::MaterialOverride>();
      for (auto &&[e, ov] : view.each()) {
        ov.gpu_slot = Components::MaterialOverride::invalid_material;
        ov.dirty = true;
        ++invalidated;
      }
    }
    info("[Sim] stop(): invalidated {} editor override slots, pool reset "
         "(base_slot={})",
         invalidated, renderer->override_pool.base_slot);

    TracyMessage("Game stopped", 12);
    active_scene = editor_scene.get();
    editor_state.active_scene = active_scene;
    runtime_scene.reset();
    editor_state.selected = entt::null;
    editor_state.cache_dirty = true;
    for (auto &panel : panels)
      panel->on_stop();

    info("[Sim] stop(): complete");
  });

  // TODO: This should maybe not be necessary, but whatever.
  vkDeviceWaitIdle(context->device);
}

auto Dockforge::unload_scene() -> void {
  if (!sim_state.in<sim::S::Editing>())
    return;

  info(
      "[Sim] unload_scene(): override_pool next={} cap={} free={} base_slot={}",
      renderer->override_pool.next, renderer->override_pool.capacity,
      renderer->override_pool.free_slots.size(),
      renderer->override_pool.base_slot);

  if (script_engine && script_engine->loaded()) {
    info("[Sim] unload_scene(): calling on_scene_unload");
    script_engine->on_scene_unload(editor_scene.get());
  }

  u32 freed = 0;
  {
    auto view = editor_scene->registry().view<Components::MaterialOverride>();
    for (auto &&[e, ov] : view.each()) {
      if (ov.gpu_slot != Components::MaterialOverride::invalid_material) {
        renderer->override_pool.free(ov.gpu_slot);
        ++freed;
      }
    }
  }
  info("[Sim] unload_scene(): freed {} override slots, resetting pool", freed);
  renderer->override_pool.next = 0;
  renderer->override_pool.free_slots.clear();
  renderer->override_pool.needs_grow = false;

  editor_scene->registry().clear();
  info("[Sim] unload_scene(): registry cleared, flat_prim_table={}",
       renderer->flat_prim_table.size());

  editor_state.selected = entt::null;
  editor_state.cache_dirty = true;
  editor_state.hierarchy_dirty = true;
}

auto Dockforge::reload_scene() -> void {
  info("[Sim] reload_scene(): begin");
  unload_scene();
  if (script_engine && script_engine->loaded()) {
    info("[Sim] reload_scene(): calling on_scene_load");
    script_engine->on_scene_load(editor_scene.get(), *asset_loader);
  }
  info("[Sim] reload_scene(): complete");
}

void Dockforge::animation_state_update(float ts) {
  ZoneScopedNC("Animation state update", 0xFF6611);
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

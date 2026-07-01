#include <dockforge/dockforge.hpp>

#include <dockforge/editor_camera.hpp>

#include <dockyard/animation.hpp>
#include <dockyard/asset_loader.hpp>
#include <dockyard/binary_stream.hpp>
#include <dockyard/components.hpp>
#include <dockyard/physics_world.hpp>
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
  if (physics_world) {
    physics_world->step(step_dt);
    physics_world->sync_transforms_out(*active_scene);
  }
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

    physics_world = std::make_unique<PhysicsWorld>();

    u32 bodies_created = 0;
    for (auto &&[entity, transform, collider, rigid_body] :
         runtime_scene
             ->view<Components::Transform, Components::Collider,
                   Components::RigidBody>()
             .each()) {
      const MeshAsset *mesh_asset = nullptr;
      if (collider.shape == Components::ColliderShape::Mesh &&
          collider.mesh_source_path.valid()) {
        auto result = asset_loader->load_mesh(
            collider.mesh_source_path.value(), /*retain_collision_geometry=*/true);
        if (result) {
          mesh_asset = renderer->get_mesh(*result);
        } else {
          warn("[Sim] play(): failed to resolve mesh collider '{}': {}",
               collider.mesh_source_path.view(), result.error());
        }
      }

      MeshCollisionGeometry mesh_geometry{};
      const MeshCollisionGeometry *mesh_geometry_ptr = nullptr;
      if (mesh_asset != nullptr) {
        mesh_geometry.positions = mesh_asset->collision_positions;
        mesh_geometry.indices = mesh_asset->collision_indices;
        mesh_geometry_ptr = &mesh_geometry;
      }

      const auto accessor = transform.get();
      const BodyTransform body_transform{.position = accessor.position,
                                         .rotation = accessor.rotation,
                                         .scale = accessor.scale};
      const u64 id = physics_world->create_rigid_body(collider, rigid_body,
                                                       body_transform,
                                                       mesh_geometry_ptr);
      rigid_body.runtime_id = id;
      collider.runtime_id = id;
      if (id != 0)
        ++bodies_created;

      if (mesh_asset != nullptr && !mesh_asset->collision_positions.empty()) {
        // mesh_aabb merges each primitive's LOCAL (pre-node-transform) AABB
        // and is unrelated to the physics mesh — measure the actual retained
        // collision geometry (already node-transform-baked by flatten_nodes)
        // instead, scaled by this entity's own top-level Transform to match
        // what create_rigid_body's setLocalScaling applies.
        AABB collision_aabb = AABB::create();
        for (const auto &vtx : mesh_asset->collision_positions)
          collision_aabb.update(glm::vec3{vtx.position[0], vtx.position[1],
                                          vtx.position[2]} *
                                accessor.scale);
        info("[Sim] play(): mesh collider entity {:x} collision AABB "
             "min=({}, {}, {}) max=({}, {}, {}), transform pos=({}, {}, {}) "
             "scale=({}, {}, {})",
             static_cast<u32>(entity), collision_aabb.get_min().x,
             collision_aabb.get_min().y, collision_aabb.get_min().z,
             collision_aabb.get_max().x, collision_aabb.get_max().y,
             collision_aabb.get_max().z, accessor.position.x,
             accessor.position.y, accessor.position.z, accessor.scale.x,
             accessor.scale.y, accessor.scale.z);
      }
    }

    u32 constraints_created = 0;
    for (auto &&[entity, constraint] :
         runtime_scene->view<Components::Constraint>().each()) {
      u64 id_a = 0;
      u64 id_b = 0;
      if (const auto *rb_a =
              runtime_scene->registry().try_get<Components::RigidBody>(
                  constraint.body_a))
        id_a = rb_a->runtime_id;
      if (const auto *rb_b =
              runtime_scene->registry().try_get<Components::RigidBody>(
                  constraint.body_b))
        id_b = rb_b->runtime_id;

      constraint.runtime_id =
          physics_world->create_constraint(constraint, id_a, id_b);
      if (constraint.runtime_id != 0)
        ++constraints_created;
    }
    info("[Sim] play(): physics world created {} rigid bodies, {} constraints",
         bodies_created, constraints_created);

    auto cam_entity = runtime_scene->make("Player Camera");
    cam_entity.emplace<Components::Camera>(editor_camera->camera);
    cam_entity.emplace<Components::FirstPersonController>();
    auto &char_ctrl = cam_entity.emplace<Components::CharacterController>();
    const auto &spawn_pos = cam_entity.get<Components::Camera>().position;
    char_ctrl.runtime_id =
        physics_world->create_character_controller(char_ctrl, spawn_pos);
    info("[Sim] play(): player spawn=({}, {}, {}) runtime_id={}",
         spawn_pos.x, spawn_pos.y, spawn_pos.z, char_ctrl.runtime_id);
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
    physics_world.reset();
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

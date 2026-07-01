#include <dockyard/physics_world.hpp>

#include <dockyard/components.hpp>
#include <dockyard/device_geometry.hpp>
#include <dockyard/log.hpp>
#include <dockyard/scene.hpp>

#include <btBulletDynamicsCommon.h>

#include <BulletCollision/CollisionDispatch/btGhostObject.h>
#include <BulletCollision/CollisionShapes/btBvhTriangleMeshShape.h>
#include <BulletCollision/CollisionShapes/btTriangleIndexVertexArray.h>
#include <BulletDynamics/ConstraintSolver/btFixedConstraint.h>
#include <BulletDynamics/ConstraintSolver/btHingeConstraint.h>
#include <BulletDynamics/ConstraintSolver/btPoint2PointConstraint.h>

#include <algorithm>
#include <unordered_map>

namespace dy {

namespace {

constexpr glm::vec3 gravity{0.0F, -9.81F, 0.0F};

// Tunables for the hand-rolled kinematic character controller (see
// PhysicsWorld::Impl::move_horizontal_with_stepup / move_vertical). Bullet's
// own btKinematicCharacterController was replaced here after proving (via
// isolated unit tests) that it rests a capsule at floor_top + radius instead
// of floor_top + radius + height/2 — effectively ignoring the capsule's
// height. Everything below is built on btCollisionWorld::convexSweepTest,
// the same primitive that correctly resolves regular dynamic rigid bodies.
constexpr f32 character_skin_width = 0.02F;      // gap kept off surfaces
constexpr int character_max_slide_iterations = 4; // corner/wedge resolution passes
constexpr f32 character_terminal_velocity = 55.0F; // m/s downward clamp
constexpr f32 character_max_step_dt = 1.0F / 30.0F; // hitch guard
constexpr f32 character_ground_probe_distance = 0.05F;
constexpr f32 character_max_slope_cosine = 0.7071F; // ~45 degrees; steeper counts as a wall
constexpr f32 character_sweep_epsilon = 1e-4F;

// Sweep callback that ignores hits against the sweeping object itself.
struct ClosestNotMeSweepCallback : btCollisionWorld::ClosestConvexResultCallback {
  const btCollisionObject *me;
  explicit ClosestNotMeSweepCallback(const btCollisionObject *self)
      : btCollisionWorld::ClosestConvexResultCallback({0.0F, 0.0F, 0.0F},
                                                      {0.0F, 0.0F, 0.0F}),
        me(self) {}
  auto addSingleResult(btCollisionWorld::LocalConvexResult &convex_result,
                       bool normal_in_world_space) -> btScalar override {
    if (convex_result.m_hitCollisionObject == me)
      return 1.0F;
    return ClosestConvexResultCallback::addSingleResult(convex_result,
                                                        normal_in_world_space);
  }
};

// Removes the component of `v` pointing into surface `n` (Quake-style
// velocity clipping for a "collide and slide" response).
[[nodiscard]] auto clip_velocity(const glm::vec3 &v, const glm::vec3 &n) -> glm::vec3 {
  return v - n * glm::dot(v, n);
}

[[nodiscard]] auto to_bt(const glm::vec3 &v) -> btVector3 {
  return {v.x, v.y, v.z};
}
[[nodiscard]] auto to_glm(const btVector3 &v) -> glm::vec3 {
  return {v.x(), v.y(), v.z()};
}
[[nodiscard]] auto to_bt(const glm::quat &q) -> btQuaternion {
  return {q.x, q.y, q.z, q.w};
}
[[nodiscard]] auto to_glm(const btQuaternion &q) -> glm::quat {
  return {q.w(), q.x(), q.y(), q.z()};
}
[[nodiscard]] auto to_bt(const BodyTransform &t) -> btTransform {
  btTransform xform;
  xform.setIdentity();
  xform.setOrigin(to_bt(t.position));
  xform.setRotation(to_bt(t.rotation));
  return xform;
}

// Result of sweeping the character capsule by a displacement: how far it got
// (as a fraction of the requested displacement, already pulled back by
// character_skin_width so the capsule never ends up embedded) and, if it hit
// something, the surface normal at the point of contact.
struct SweepResult {
  f32 fraction = 1.0F;
  glm::vec3 normal{0.0F};
  bool hit = false;
};

} // namespace

struct PhysicsWorld::Impl {
  std::unique_ptr<btDefaultCollisionConfiguration> collision_config;
  std::unique_ptr<btCollisionDispatcher> dispatcher;
  std::unique_ptr<btDbvtBroadphase> broadphase;
  std::unique_ptr<btSequentialImpulseConstraintSolver> solver;
  std::unique_ptr<btGhostPairCallback> ghost_pair_callback;
  std::unique_ptr<btDiscreteDynamicsWorld> world;

  struct RigidBodyEntry {
    std::unique_ptr<btCollisionShape> shape;
    std::unique_ptr<btTriangleIndexVertexArray> mesh_interface; // Mesh shape only
    std::unique_ptr<btMotionState> motion_state;
    std::unique_ptr<btRigidBody> body;
  };
  struct CharacterEntry {
    std::unique_ptr<btCapsuleShape> shape;
    std::unique_ptr<btPairCachingGhostObject> ghost; // kept for broadphase presence
    f32 radius = 0.3F;
    f32 height = 1.8F; // Bullet convention: distance between cap-sphere centers
    f32 step_height = 0.35F;
    f32 jump_speed = 5.0F;
    glm::vec3 velocity{0.0F}; // persistent world-space velocity, integrated per frame
    bool on_ground = false;
  };
  struct ConstraintEntry {
    std::unique_ptr<btTypedConstraint> constraint;
  };

  std::unordered_map<u64, RigidBodyEntry> bodies;
  std::unordered_map<u64, CharacterEntry> characters;
  std::unordered_map<u64, ConstraintEntry> constraints;
  u64 next_id = 1;

  Impl() {
    collision_config = std::make_unique<btDefaultCollisionConfiguration>();
    dispatcher = std::make_unique<btCollisionDispatcher>(collision_config.get());
    broadphase = std::make_unique<btDbvtBroadphase>();
    solver = std::make_unique<btSequentialImpulseConstraintSolver>();
    world = std::make_unique<btDiscreteDynamicsWorld>(
        dispatcher.get(), broadphase.get(), solver.get(), collision_config.get());
    world->setGravity(to_bt(gravity));

    ghost_pair_callback = std::make_unique<btGhostPairCallback>();
    broadphase->getOverlappingPairCache()->setInternalGhostPairCallback(
        ghost_pair_callback.get());
  }

  ~Impl() {
    // Constraints reference bodies; characters reference the world via
    // addCollisionObject. Tear down in the reverse order things were
    // registered so Bullet never dereferences an already-freed object.
    for (auto &[id, entry] : constraints)
      world->removeConstraint(entry.constraint.get());
    constraints.clear();

    for (auto &[id, entry] : characters)
      world->removeCollisionObject(entry.ghost.get());
    characters.clear();

    for (auto &[id, entry] : bodies)
      world->removeRigidBody(entry.body.get());
    bodies.clear();
  }

  [[nodiscard]] auto allocate_id() -> u64 { return next_id++; }

  // Sweeps the character capsule from world position `from` by `motion` (a
  // displacement, not a target position).
  [[nodiscard]] auto sweep_capsule(const CharacterEntry &entry, const glm::vec3 &from,
                                   const glm::vec3 &motion) const -> SweepResult;

  // Iterative "collide and slide": sweeps `motion`, and on a hit, advances to
  // the contact point and clips the remaining motion against the hit normal
  // (zeroed on Y, since this is the horizontal-only pass), repeating up to
  // character_max_slide_iterations times to resolve corners.
  [[nodiscard]] auto collide_and_slide(CharacterEntry &entry, glm::vec3 pos,
                                       glm::vec3 motion) const -> glm::vec3;

  // Horizontal movement for one frame. Tries a direct collide-and-slide move;
  // if that made little progress while grounded, retries as a step: lift by
  // step_height, slide forward, then drop back down, keeping the result only
  // if it made more progress and actually landed on something.
  [[nodiscard]] auto move_horizontal_with_stepup(CharacterEntry &entry, glm::vec3 pos,
                                                 const glm::vec3 &disp) const
      -> glm::vec3;

  // Vertical movement (gravity fall / jump) for one frame. Updates
  // entry.on_ground and zeroes entry.velocity.y on a floor/ceiling hit.
  [[nodiscard]] auto move_vertical(CharacterEntry &entry, glm::vec3 pos,
                                   const glm::vec3 &disp) const -> glm::vec3;
};

auto PhysicsWorld::Impl::sweep_capsule(const CharacterEntry &entry, const glm::vec3 &from,
                                       const glm::vec3 &motion) const -> SweepResult {
  const f32 distance = glm::length(motion);
  if (distance < character_sweep_epsilon)
    return {};

  btTransform from_transform;
  from_transform.setIdentity();
  from_transform.setOrigin(to_bt(from));
  btTransform to_transform;
  to_transform.setIdentity();
  to_transform.setOrigin(to_bt(from + motion));

  ClosestNotMeSweepCallback callback(entry.ghost.get());
  callback.m_collisionFilterGroup = btBroadphaseProxy::CharacterFilter;
  callback.m_collisionFilterMask =
      btBroadphaseProxy::StaticFilter | btBroadphaseProxy::DefaultFilter;

  world->convexSweepTest(entry.shape.get(), from_transform, to_transform, callback,
                         character_skin_width);
  if (!callback.hasHit())
    return {};

  // Pull back by the skin width so the capsule never ends up embedded in the
  // surface it just hit.
  const f32 fraction =
      std::max(0.0F, callback.m_closestHitFraction - character_skin_width / distance);
  return {.fraction = fraction, .normal = to_glm(callback.m_hitNormalWorld), .hit = true};
}

auto PhysicsWorld::Impl::collide_and_slide(CharacterEntry &entry, glm::vec3 pos,
                                           glm::vec3 motion) const -> glm::vec3 {
  for (int i = 0; i < character_max_slide_iterations &&
                 glm::length(motion) > character_sweep_epsilon;
       ++i) {
    const SweepResult sweep = sweep_capsule(entry, pos, motion);
    pos += motion * sweep.fraction;
    if (!sweep.hit)
      break;
    glm::vec3 remaining = motion * (1.0F - sweep.fraction);
    motion = clip_velocity(remaining, sweep.normal);
    motion.y = 0.0F; // this pass is horizontal-only
  }
  return pos;
}

auto PhysicsWorld::Impl::move_horizontal_with_stepup(CharacterEntry &entry, glm::vec3 pos,
                                                     const glm::vec3 &disp) const
    -> glm::vec3 {
  if (glm::length(disp) < character_sweep_epsilon)
    return pos;

  const glm::vec3 slid_pos = collide_and_slide(entry, pos, disp);

  const f32 wanted = glm::length(glm::vec3{disp.x, 0.0F, disp.z});
  const f32 got =
      glm::length(glm::vec3{slid_pos.x - pos.x, 0.0F, slid_pos.z - pos.z});
  if (!entry.on_ground || got >= wanted - character_sweep_epsilon)
    return slid_pos;

  // Blocked while grounded: try stepping over it. Lift by step_height, then
  // check whether there's a walkable surface up there. If the lift itself is
  // even partially blocked (something low overhead), bail out rather than
  // sliding forward from a half-lifted position.
  const SweepResult up = sweep_capsule(entry, pos, {0.0F, entry.step_height, 0.0F});
  if (up.hit)
    return slid_pos;
  const glm::vec3 lifted = pos + glm::vec3{0.0F, entry.step_height, 0.0F};

  // This frame's real `disp` can be much shorter than the capsule's radius
  // (e.g. one frame of walking speed), which isn't enough horizontal travel
  // to clear the obstruction's corner — the capsule would end up straddling
  // the edge, the drop sweep would catch that edge instead of the flat
  // surface beyond it, and the character would get wedged there permanently.
  // So the horizontal slide at the lifted height always travels a fixed
  // "clear the corner" distance (twice the radius: enough to go from "just
  // touching the corner" to "fully past it") instead of just `disp` — a
  // small, one-time forward nudge on the frame a step-up is confirmed,
  // rather than trying to inch through the danger zone smoothly.
  const glm::vec2 horiz_dir = glm::vec2{disp.x, disp.z};
  const f32 step_distance = 2.0F * entry.radius + character_skin_width;
  const glm::vec2 step_dir = glm::normalize(horiz_dir);
  const glm::vec3 step_disp{step_dir.x * step_distance, 0.0F, step_dir.y * step_distance};

  const glm::vec3 stepped = collide_and_slide(entry, lifted, step_disp);
  const SweepResult down =
      sweep_capsule(entry, stepped, {0.0F, -entry.step_height, 0.0F});
  if (!down.hit || down.normal.y <= character_max_slope_cosine)
    return slid_pos; // nothing walkable up there (open air or another edge)

  return stepped + glm::vec3{0.0F, -entry.step_height * down.fraction, 0.0F};
}

auto PhysicsWorld::Impl::move_vertical(CharacterEntry &entry, glm::vec3 pos,
                                       const glm::vec3 &disp) const -> glm::vec3 {
  entry.on_ground = false;

  if (std::abs(disp.y) > character_sweep_epsilon) {
    const SweepResult sweep = sweep_capsule(entry, pos, disp);
    pos += disp * sweep.fraction;
    if (sweep.hit) {
      if (sweep.normal.y > character_max_slope_cosine && entry.velocity.y <= 0.0F) {
        entry.on_ground = true;
        entry.velocity.y = 0.0F;
      } else if (sweep.normal.y < -character_max_slope_cosine && entry.velocity.y > 0.0F) {
        entry.velocity.y = 0.0F; // hit a ceiling
      }
    }
  }

  // Probe a little further down to keep on_ground accurate even when
  // standing still (disp.y is then too small to have swept far enough).
  if (!entry.on_ground) {
    const SweepResult probe = sweep_capsule(
        entry, pos, {0.0F, -(character_skin_width + character_ground_probe_distance), 0.0F});
    if (probe.hit && probe.normal.y > character_max_slope_cosine) {
      entry.on_ground = true;
      if (entry.velocity.y < 0.0F)
        entry.velocity.y = 0.0F;
    }
  }

  return pos;
}

PhysicsWorld::PhysicsWorld() : impl(std::make_unique<Impl>()) {}
PhysicsWorld::~PhysicsWorld() = default;

auto PhysicsWorld::step(f32 dt) -> void {
  impl->world->stepSimulation(dt, 10);
}

auto PhysicsWorld::sync_transforms_out(Scene &scene) -> void {
  for (auto &&[entity, transform, rigid_body] :
       scene.view<Components::Transform, Components::RigidBody>().each()) {
    if (rigid_body.runtime_id == 0 || rigid_body.kinematic ||
        rigid_body.mass <= 0.0F)
      continue;

    const auto it = impl->bodies.find(rigid_body.runtime_id);
    if (it == impl->bodies.end())
      continue;

    btTransform xform;
    it->second.motion_state->getWorldTransform(xform);

    auto mut = transform.mut();
    mut.position = to_glm(xform.getOrigin());
    mut.rotation = to_glm(xform.getRotation());
  }
}

namespace {

[[nodiscard]] auto make_shape(const Components::Collider &collider,
                              const MeshCollisionGeometry *mesh_geometry,
                              std::unique_ptr<btTriangleIndexVertexArray> &mesh_interface_out)
    -> std::unique_ptr<btCollisionShape> {
  switch (collider.shape) {
  case Components::ColliderShape::Box:
    return std::make_unique<btBoxShape>(to_bt(collider.half_extents));
  case Components::ColliderShape::Sphere:
    return std::make_unique<btSphereShape>(collider.radius);
  case Components::ColliderShape::Capsule:
    return std::make_unique<btCapsuleShape>(collider.radius, collider.height);
  case Components::ColliderShape::Mesh:
    if (mesh_geometry == nullptr || mesh_geometry->positions.empty() ||
        mesh_geometry->indices.empty()) {
      error("PhysicsWorld: mesh collider requested with no retained collision "
            "geometry");
      return nullptr;
    }
    info("PhysicsWorld: building mesh collider from {} vertices, {} indices "
         "({} triangles)",
         mesh_geometry->positions.size(), mesh_geometry->indices.size(),
         mesh_geometry->indices.size() / 3);
    mesh_interface_out = std::make_unique<btTriangleIndexVertexArray>(
        static_cast<int>(mesh_geometry->indices.size() / 3),
        const_cast<int *>(
            reinterpret_cast<const int *>(mesh_geometry->indices.data())),
        3 * static_cast<int>(sizeof(u32)),
        static_cast<int>(mesh_geometry->positions.size()),
        const_cast<btScalar *>(
            reinterpret_cast<const btScalar *>(mesh_geometry->positions.data())),
        static_cast<int>(sizeof(PositionOnlyVertex)));
    return std::make_unique<btBvhTriangleMeshShape>(mesh_interface_out.get(),
                                                    /*useQuantizedAabbCompression=*/true);
  }
  return nullptr;
}

} // namespace

auto PhysicsWorld::create_rigid_body(const Components::Collider &collider,
                                     const Components::RigidBody &body,
                                     const BodyTransform &transform,
                                     const MeshCollisionGeometry *mesh_geometry)
    -> u64 {
  if (collider.shape == Components::ColliderShape::Mesh && body.mass > 0.0F &&
      !body.kinematic) {
    error("PhysicsWorld: dynamic rigid bodies cannot use a Mesh collider "
          "(mass={})",
          body.mass);
    return 0;
  }

  std::unique_ptr<btTriangleIndexVertexArray> mesh_interface;
  auto shape = make_shape(collider, mesh_geometry, mesh_interface);
  if (!shape)
    return 0;

  // Apply the entity's Transform scale to the shape before computing inertia,
  // so a scaled-down (or up) level mesh gets a matching collision volume
  // instead of the raw, unscaled shape dimensions.
  shape->setLocalScaling(to_bt(transform.scale));

  const btScalar mass = body.kinematic ? 0.0F : body.mass;
  btVector3 local_inertia{0.0F, 0.0F, 0.0F};
  if (mass > 0.0F)
    shape->calculateLocalInertia(mass, local_inertia);

  auto motion_state = std::make_unique<btDefaultMotionState>(to_bt(transform));
  btRigidBody::btRigidBodyConstructionInfo info{mass, motion_state.get(),
                                                shape.get(), local_inertia};
  info.m_friction = body.friction;
  info.m_restitution = body.restitution;

  auto rigid_body = std::make_unique<btRigidBody>(info);
  if (body.kinematic) {
    rigid_body->setCollisionFlags(rigid_body->getCollisionFlags() |
                                  btCollisionObject::CF_KINEMATIC_OBJECT);
    rigid_body->setActivationState(DISABLE_DEACTIVATION);
  }

  const u64 id = impl->allocate_id();
  impl->world->addRigidBody(rigid_body.get());

  auto &entry = impl->bodies[id];
  entry.shape = std::move(shape);
  entry.mesh_interface = std::move(mesh_interface);
  entry.motion_state = std::move(motion_state);
  entry.body = std::move(rigid_body);
  return id;
}

auto PhysicsWorld::destroy_rigid_body(u64 runtime_id) -> void {
  const auto it = impl->bodies.find(runtime_id);
  if (it == impl->bodies.end())
    return;
  impl->world->removeRigidBody(it->second.body.get());
  impl->bodies.erase(it);
}

auto PhysicsWorld::set_kinematic_transform(u64 runtime_id,
                                          const BodyTransform &transform)
    -> void {
  const auto it = impl->bodies.find(runtime_id);
  if (it == impl->bodies.end())
    return;
  it->second.motion_state->setWorldTransform(to_bt(transform));
}

auto PhysicsWorld::create_character_controller(
    const Components::CharacterController &controller, const glm::vec3 &position)
    -> u64 {
  auto shape =
      std::make_unique<btCapsuleShape>(controller.radius, controller.height);

  auto ghost = std::make_unique<btPairCachingGhostObject>();
  btTransform start;
  start.setIdentity();
  start.setOrigin(to_bt(position));
  ghost->setWorldTransform(start);
  ghost->setCollisionShape(shape.get());
  ghost->setCollisionFlags(btCollisionObject::CF_CHARACTER_OBJECT);

  // No btKinematicCharacterController/addAction here: it rested capsules at
  // floor_top + radius instead of floor_top + radius + height/2, ignoring the
  // capsule's height entirely (verified in isolated unit tests, independent
  // of floor size, gravity, and capsule height/shape). Movement is instead
  // resolved by hand in PhysicsWorld::move_character via convexSweepTest,
  // which is proven correct — it's the same primitive that backs normal
  // dynamic rigid bodies.
  impl->world->addCollisionObject(ghost.get(),
                                  btBroadphaseProxy::CharacterFilter,
                                  btBroadphaseProxy::StaticFilter |
                                      btBroadphaseProxy::DefaultFilter);

  const u64 id = impl->allocate_id();
  auto &entry = impl->characters[id];
  entry.shape = std::move(shape);
  entry.ghost = std::move(ghost);
  entry.radius = controller.radius;
  entry.height = controller.height;
  entry.step_height = controller.step_height;
  entry.jump_speed = controller.jump_speed;
  entry.velocity = glm::vec3{0.0F};
  entry.on_ground = false;
  return id;
}

auto PhysicsWorld::destroy_character_controller(u64 runtime_id) -> void {
  const auto it = impl->characters.find(runtime_id);
  if (it == impl->characters.end())
    return;
  impl->world->removeCollisionObject(it->second.ghost.get());
  impl->characters.erase(it);
}

auto PhysicsWorld::move_character(u64 runtime_id, const glm::vec3 &walk_direction,
                                  bool jump, f32 dt) -> void {
  const auto it = impl->characters.find(runtime_id);
  if (it == impl->characters.end())
    return;
  auto &entry = it->second;

  // Hitch guard: a single continuous sweep can't tunnel regardless of dt,
  // but an oversized step-up lift/drop on a huge dt spike could behave
  // oddly, so cap it defensively.
  dt = std::min(dt, character_max_step_dt);

  if (jump && entry.on_ground)
    entry.velocity.y = entry.jump_speed;
  else
    entry.velocity.y += gravity.y * dt;
  entry.velocity.y = std::max(entry.velocity.y, -character_terminal_velocity);

  glm::vec3 pos = to_glm(entry.ghost->getWorldTransform().getOrigin());

  const glm::vec3 horizontal_disp =
      glm::vec3{walk_direction.x, 0.0F, walk_direction.z} * dt;
  pos = impl->move_horizontal_with_stepup(entry, pos, horizontal_disp);

  const glm::vec3 vertical_disp{0.0F, entry.velocity.y * dt, 0.0F};
  pos = impl->move_vertical(entry, pos, vertical_disp);

  btTransform xform;
  xform.setIdentity();
  xform.setOrigin(to_bt(pos));
  entry.ghost->setWorldTransform(xform);
}

auto PhysicsWorld::character_position(u64 runtime_id) const -> glm::vec3 {
  const auto it = impl->characters.find(runtime_id);
  if (it == impl->characters.end())
    return glm::vec3{0.0F};
  return to_glm(it->second.ghost->getWorldTransform().getOrigin());
}

auto PhysicsWorld::character_on_ground(u64 runtime_id) const -> bool {
  const auto it = impl->characters.find(runtime_id);
  if (it == impl->characters.end())
    return false;
  return it->second.on_ground;
}

auto PhysicsWorld::create_constraint(const Components::Constraint &constraint,
                                     u64 body_a_runtime_id,
                                     u64 body_b_runtime_id) -> u64 {
  const auto a_it = impl->bodies.find(body_a_runtime_id);
  if (a_it == impl->bodies.end()) {
    error("PhysicsWorld: create_constraint: body_a runtime_id {} not found",
          body_a_runtime_id);
    return 0;
  }
  btRigidBody &body_a = *a_it->second.body;

  const auto b_it = impl->bodies.find(body_b_runtime_id);
  btRigidBody *body_b = (b_it != impl->bodies.end()) ? b_it->second.body.get() : nullptr;

  std::unique_ptr<btTypedConstraint> typed_constraint;
  switch (constraint.type) {
  case Components::ConstraintType::Point:
    typed_constraint = body_b != nullptr
        ? std::make_unique<btPoint2PointConstraint>(
              body_a, *body_b, to_bt(constraint.pivot_a), to_bt(constraint.pivot_b))
        : std::make_unique<btPoint2PointConstraint>(body_a, to_bt(constraint.pivot_a));
    break;
  case Components::ConstraintType::Hinge:
    typed_constraint = body_b != nullptr
        ? std::make_unique<btHingeConstraint>(
              body_a, *body_b, to_bt(constraint.pivot_a), to_bt(constraint.pivot_b),
              to_bt(constraint.axis_a), to_bt(constraint.axis_b))
        : std::make_unique<btHingeConstraint>(body_a, to_bt(constraint.pivot_a),
                                              to_bt(constraint.axis_a));
    break;
  case Components::ConstraintType::Fixed: {
    btTransform frame_a;
    frame_a.setIdentity();
    frame_a.setOrigin(to_bt(constraint.pivot_a));
    if (body_b != nullptr) {
      btTransform frame_b;
      frame_b.setIdentity();
      frame_b.setOrigin(to_bt(constraint.pivot_b));
      typed_constraint =
          std::make_unique<btFixedConstraint>(body_a, *body_b, frame_a, frame_b);
    } else {
      static btRigidBody fixed_ground_body{0.0F, nullptr, nullptr};
      typed_constraint = std::make_unique<btFixedConstraint>(
          body_a, fixed_ground_body, frame_a, frame_a);
    }
    break;
  }
  }

  if (!typed_constraint)
    return 0;

  const u64 id = impl->allocate_id();
  impl->world->addConstraint(typed_constraint.get(), /*disableCollisionsBetweenLinkedBodies=*/true);
  impl->constraints[id].constraint = std::move(typed_constraint);
  return id;
}

auto PhysicsWorld::destroy_constraint(u64 runtime_id) -> void {
  const auto it = impl->constraints.find(runtime_id);
  if (it == impl->constraints.end())
    return;
  impl->world->removeConstraint(it->second.constraint.get());
  impl->constraints.erase(it);
}

} // namespace dy

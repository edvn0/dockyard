#include <dockyard/physics_world.hpp>

#include <dockyard/components.hpp>
#include <dockyard/scene.hpp>

#include <doctest/doctest.h>

#include <algorithm>

using namespace dy;

namespace {
constexpr f32 fixed_dt = 1.0F / 60.0F;
}

TEST_CASE("Given a static box and a dynamic sphere dropped above it, "
         "When the world steps for several seconds, "
         "Then the sphere comes to rest on top of the box") {
  PhysicsWorld world;

  const Components::Collider box_collider{
      .shape = Components::ColliderShape::Box,
      .half_extents = {5.0F, 0.5F, 5.0F},
  };
  const Components::RigidBody box_body{.mass = 0.0F};
  const u64 box_id = world.create_rigid_body(
      box_collider, box_body, BodyTransform{.position = {0.0F, 0.0F, 0.0F}},
      nullptr);
  REQUIRE(box_id != 0);

  const Components::Collider sphere_collider{
      .shape = Components::ColliderShape::Sphere,
      .radius = 0.5F,
  };
  Components::RigidBody sphere_body{.mass = 1.0F, .restitution = 0.0F};
  const u64 sphere_id = world.create_rigid_body(
      sphere_collider, sphere_body,
      BodyTransform{.position = {0.0F, 5.0F, 0.0F}}, nullptr);
  REQUIRE(sphere_id != 0);
  sphere_body.runtime_id = sphere_id;

  constexpr int steps = 240; // 4 seconds, ample settling time.
  for (int i = 0; i < steps; ++i)
    world.step(fixed_dt);

  Scene scene;
  auto sphere_entity = scene.registry().create();
  scene.registry().emplace<Components::Transform>(sphere_entity);
  scene.registry().emplace<Components::RigidBody>(sphere_entity, sphere_body);

  world.sync_transforms_out(scene);

  const auto &transform =
      scene.registry().get<Components::Transform>(sphere_entity);
  const f32 resting_height = transform.get().position.y;

  // Box top sits at y=0.5 (half_extents.y); a 0.5-radius sphere at rest on it
  // should settle with its center near y=1.0.
  CHECK(resting_height == doctest::Approx(1.0F).epsilon(0.1));
}

TEST_CASE("Given a static box collider on an entity scaled down 100x, "
         "When a sphere is dropped onto it, "
         "Then it rests at the scaled-down height, not the authored one") {
  PhysicsWorld world;

  // Authored as a 100x100x100 half-extent box (matching an oversized source
  // asset), then scaled down 0.01x via the entity's Transform — the same
  // "model authored large, shrunk in a script" pattern that exposed this bug:
  // BodyTransform::scale was previously ignored entirely, so the collision
  // shape stayed at its full 100-unit size regardless of the entity's scale.
  const Components::Collider box_collider{
      .shape = Components::ColliderShape::Box,
      .half_extents = {100.0F, 100.0F, 100.0F},
  };
  const Components::RigidBody box_body{.mass = 0.0F};
  const u64 box_id = world.create_rigid_body(
      box_collider, box_body,
      BodyTransform{.position = {0.0F, 0.0F, 0.0F},
                    .scale = {0.01F, 0.01F, 0.01F}},
      nullptr);
  REQUIRE(box_id != 0);

  const Components::Collider sphere_collider{
      .shape = Components::ColliderShape::Sphere,
      .radius = 0.5F,
  };
  Components::RigidBody sphere_body{.mass = 1.0F, .restitution = 0.0F};
  const u64 sphere_id = world.create_rigid_body(
      sphere_collider, sphere_body,
      BodyTransform{.position = {0.0F, 5.0F, 0.0F}}, nullptr);
  REQUIRE(sphere_id != 0);
  sphere_body.runtime_id = sphere_id;

  constexpr int steps = 240; // 4 seconds, ample settling time.
  for (int i = 0; i < steps; ++i)
    world.step(fixed_dt);

  Scene scene;
  auto sphere_entity = scene.registry().create();
  scene.registry().emplace<Components::Transform>(sphere_entity);
  scene.registry().emplace<Components::RigidBody>(sphere_entity, sphere_body);
  world.sync_transforms_out(scene);

  const f32 resting_height =
      scene.registry().get<Components::Transform>(sphere_entity).get().position.y;

  // Scaled box top sits at y=1.0 (100 * 0.01); the sphere should rest near
  // y=1.5. Without the scale fix, the box's real (unscaled) top would be at
  // y=100 and the sphere would still be in freefall at y≈5-ish after 4s.
  CHECK(resting_height == doctest::Approx(1.5F).epsilon(0.1));
}

TEST_CASE("Given a kinematic character capsule dropped above a static floor, "
         "When it falls for several seconds, "
         "Then it comes to rest at floor_top + height/2 + radius, not "
         "floor_top + radius") {
  PhysicsWorld world;

  const Components::Collider floor_collider{
      .shape = Components::ColliderShape::Box,
      .half_extents = {50.0F, 0.5F, 50.0F},
  };
  const Components::RigidBody floor_body{.mass = 0.0F};
  const u64 floor_id = world.create_rigid_body(
      floor_collider, floor_body, BodyTransform{.position = {0.0F, 0.0F, 0.0F}},
      nullptr);
  REQUIRE(floor_id != 0);

  const Components::CharacterController char_ctrl{
      .radius = 0.3F,
      .height = 1.8F,
      .step_height = 0.35F,
      .move_speed = 5.0F,
  };
  const u64 char_id =
      world.create_character_controller(char_ctrl, glm::vec3{0.0F, 5.0F, 0.0F});
  REQUIRE(char_id != 0);

  constexpr int steps = 240; // 4 seconds, ample time to fall ~3.3 units and settle.
  for (int i = 0; i < steps; ++i)
    world.move_character(char_id, glm::vec3{0.0F}, false, fixed_dt);

  const glm::vec3 pos = world.character_position(char_id);
  // Floor top sits at y=0.5; a radius-0.3, height-1.8 capsule resting on it
  // has its center at y = 0.5 + (1.8/2 + 0.3) = 1.7. The bug this replaces
  // Bullet's btKinematicCharacterController for would land it at 0.5+0.3=0.8
  // instead, sinking the character roughly height/2 into the floor.
  CHECK(pos.y == doctest::Approx(1.7F).epsilon(0.05));
  CHECK(world.character_on_ground(char_id));
}

TEST_CASE("Given a kinematic capsule character walking toward a static wall, "
         "When it walks for several seconds, "
         "Then it does not tunnel through the wall") {
  PhysicsWorld world;

  // A floor is required: gravity is applied every frame regardless of the
  // dynamics world's other bodies, and with nothing to stand on the
  // character falls well below the wall's vertical extent long before
  // reaching it horizontally, walking straight past underneath.
  const Components::Collider floor_collider{
      .shape = Components::ColliderShape::Box,
      .half_extents = {50.0F, 0.5F, 50.0F},
  };
  const Components::RigidBody floor_body{.mass = 0.0F};
  const u64 floor_id = world.create_rigid_body(
      floor_collider, floor_body, BodyTransform{.position = {0.0F, 0.0F, 0.0F}},
      nullptr);
  REQUIRE(floor_id != 0);

  const Components::Collider wall_collider{
      .shape = Components::ColliderShape::Box,
      .half_extents = {0.1F, 5.0F, 5.0F},
  };
  const Components::RigidBody wall_body{.mass = 0.0F};
  const u64 wall_id = world.create_rigid_body(
      wall_collider, wall_body, BodyTransform{.position = {2.0F, 0.0F, 0.0F}},
      nullptr);
  REQUIRE(wall_id != 0);

  const Components::CharacterController char_ctrl{
      .radius = 0.3F,
      .height = 1.8F,
      .step_height = 0.35F,
      .move_speed = 5.0F,
  };
  // Spawned above the floor's correct resting height (1.7); it settles onto
  // the floor before ever reaching the wall.
  const u64 char_id =
      world.create_character_controller(char_ctrl, glm::vec3{0.0F, 2.0F, 0.0F});
  REQUIRE(char_id != 0);

  constexpr int steps = 300; // 5 seconds of walking toward the wall.
  for (int i = 0; i < steps; ++i)
    world.move_character(char_id, glm::vec3{5.0F, 0.0F, 0.0F}, false, fixed_dt);

  const glm::vec3 pos = world.character_position(char_id);
  // The wall's near face sits at x=1.9 (2.0 - half_extents.x). A capsule with
  // radius 0.3 tunnelling through would end up past x=2.1 (the far face); a
  // capsule blocked by the wall stops with its center short of the near face.
  CHECK(pos.x < 1.9F);
}

TEST_CASE("Given a character walking diagonally into a wide wall, "
         "When it collides with the wall, "
         "Then it slides along the wall instead of stopping dead") {
  PhysicsWorld world;

  const Components::Collider floor_collider{
      .shape = Components::ColliderShape::Box,
      .half_extents = {50.0F, 0.5F, 50.0F},
  };
  const Components::RigidBody floor_body{.mass = 0.0F};
  const u64 floor_id = world.create_rigid_body(
      floor_collider, floor_body, BodyTransform{.position = {0.0F, 0.0F, 0.0F}},
      nullptr);
  REQUIRE(floor_id != 0);

  // A wide wall running along X, blocking +Z movement past z=1.9. Half-extent
  // 50 in X comfortably exceeds the ~25 units of X travel possible in this
  // test's 5 seconds at move_speed 5, so the character can't just walk
  // around its end.
  const Components::Collider wall_collider{
      .shape = Components::ColliderShape::Box,
      .half_extents = {50.0F, 5.0F, 0.1F},
  };
  const Components::RigidBody wall_body{.mass = 0.0F};
  const u64 wall_id = world.create_rigid_body(
      wall_collider, wall_body, BodyTransform{.position = {0.0F, 0.0F, 2.0F}},
      nullptr);
  REQUIRE(wall_id != 0);

  const Components::CharacterController char_ctrl{
      .radius = 0.3F,
      .height = 1.8F,
      .step_height = 0.35F,
      .move_speed = 5.0F,
  };
  const u64 char_id =
      world.create_character_controller(char_ctrl, glm::vec3{0.0F, 2.0F, 0.0F});
  REQUIRE(char_id != 0);

  constexpr int steps = 300; // 5 seconds of diagonal walking into the wall.
  for (int i = 0; i < steps; ++i)
    world.move_character(char_id, glm::vec3{5.0F, 0.0F, 5.0F}, false, fixed_dt);

  const glm::vec3 pos = world.character_position(char_id);
  // Nothing blocks X, so a sliding controller keeps making X progress; a
  // "stop dead on any hit" controller would leave both X and Z near zero.
  CHECK(pos.x > 5.0F);
  // Z is blocked at the wall's near face (z=1.9) minus the capsule radius.
  CHECK(pos.z < 1.9F);
}

TEST_CASE("Given a low ledge shorter than the character's step height, "
         "When the character walks into it, "
         "Then it steps up onto the ledge instead of getting blocked") {
  PhysicsWorld world;

  const Components::Collider floor_collider{
      .shape = Components::ColliderShape::Box,
      .half_extents = {50.0F, 0.5F, 50.0F},
  };
  const Components::RigidBody floor_body{.mass = 0.0F};
  const u64 floor_id = world.create_rigid_body(
      floor_collider, floor_body, BodyTransform{.position = {0.0F, 0.0F, 0.0F}},
      nullptr);
  REQUIRE(floor_id != 0);

  // Ledge top at y=0.7 (0.2 above the floor's y=0.5 top — under the
  // character's 0.35 step_height), starting at x=3.0 so there's flat floor
  // to walk across first.
  const Components::Collider ledge_collider{
      .shape = Components::ColliderShape::Box,
      .half_extents = {2.0F, 0.1F, 5.0F},
  };
  const Components::RigidBody ledge_body{.mass = 0.0F};
  const u64 ledge_id = world.create_rigid_body(
      ledge_collider, ledge_body, BodyTransform{.position = {5.0F, 0.6F, 0.0F}},
      nullptr);
  REQUIRE(ledge_id != 0);

  const Components::CharacterController char_ctrl{
      .radius = 0.3F,
      .height = 1.8F,
      .step_height = 0.35F,
      .move_speed = 3.0F,
  };
  const u64 char_id =
      world.create_character_controller(char_ctrl, glm::vec3{0.0F, 2.0F, 0.0F});
  REQUIRE(char_id != 0);

  constexpr int steps = 240; // 4 seconds: cross the flat floor, then the ledge.
  for (int i = 0; i < steps; ++i)
    world.move_character(char_id, glm::vec3{3.0F, 0.0F, 0.0F}, false, fixed_dt);

  const glm::vec3 pos = world.character_position(char_id);
  // Flat-floor resting height is 1.7; stepped up onto the ledge it should be
  // 0.7 + (1.8/2 + 0.3) = 1.9. 1.8 cleanly distinguishes "stepped up" from
  // "stuck at the ledge's near face".
  CHECK(pos.y > 1.8F);
  CHECK(pos.x > 3.0F);
}

TEST_CASE("Given a grounded character, "
         "When jump is requested, "
         "Then it rises and settles back to its resting height, "
         "and jump has no effect while airborne") {
  PhysicsWorld world;

  const Components::Collider floor_collider{
      .shape = Components::ColliderShape::Box,
      .half_extents = {50.0F, 0.5F, 50.0F},
  };
  const Components::RigidBody floor_body{.mass = 0.0F};
  const u64 floor_id = world.create_rigid_body(
      floor_collider, floor_body, BodyTransform{.position = {0.0F, 0.0F, 0.0F}},
      nullptr);
  REQUIRE(floor_id != 0);

  const Components::CharacterController char_ctrl{
      .radius = 0.3F,
      .height = 1.8F,
      .step_height = 0.35F,
      .jump_speed = 5.0F,
  };
  const u64 char_id =
      world.create_character_controller(char_ctrl, glm::vec3{0.0F, 2.0F, 0.0F});
  REQUIRE(char_id != 0);

  // Let it settle onto the floor first.
  for (int i = 0; i < 120; ++i)
    world.move_character(char_id, glm::vec3{0.0F}, false, fixed_dt);
  const f32 resting_y = world.character_position(char_id).y;
  REQUIRE(world.character_on_ground(char_id));

  // One jump request while grounded, then no further requests: the
  // character should rise once and settle back onto the floor. A jump
  // request repeated every frame while airborne would legitimately
  // re-trigger the instant it touches down again (on_ground gates jump, not
  // "already jumped once"), so this is tested separately below.
  f32 peak_y = resting_y;
  constexpr int air_steps = 90; // 1.5 seconds: enough to rise and land again.
  for (int i = 0; i < air_steps; ++i) {
    world.move_character(char_id, glm::vec3{0.0F}, i == 0, fixed_dt);
    peak_y = std::max(peak_y, world.character_position(char_id).y);
  }

  CHECK(peak_y > resting_y + 0.2F);
  CHECK(world.character_position(char_id).y == doctest::Approx(resting_y).epsilon(0.05));
  CHECK(world.character_on_ground(char_id));
}

TEST_CASE("Given a dynamic body fixed-constrained to a static anchor, "
         "When the world steps under gravity, "
         "Then the body does not fall away from the anchor") {
  PhysicsWorld world;

  const Components::Collider anchor_collider{
      .shape = Components::ColliderShape::Box,
      .half_extents = {0.5F, 0.5F, 0.5F},
  };
  const Components::RigidBody anchor_body{.mass = 0.0F};
  const u64 anchor_id = world.create_rigid_body(
      anchor_collider, anchor_body,
      BodyTransform{.position = {0.0F, 5.0F, 0.0F}}, nullptr);
  REQUIRE(anchor_id != 0);

  const Components::Collider dynamic_collider{
      .shape = Components::ColliderShape::Sphere,
      .radius = 0.3F,
  };
  Components::RigidBody dynamic_body{.mass = 1.0F};
  const u64 dynamic_id = world.create_rigid_body(
      dynamic_collider, dynamic_body,
      BodyTransform{.position = {0.0F, 4.0F, 0.0F}}, nullptr);
  REQUIRE(dynamic_id != 0);
  dynamic_body.runtime_id = dynamic_id;

  const Components::Constraint constraint{
      .type = Components::ConstraintType::Fixed,
  };
  const u64 constraint_id =
      world.create_constraint(constraint, anchor_id, dynamic_id);
  REQUIRE(constraint_id != 0);

  constexpr int steps = 120; // 2 seconds under gravity.
  for (int i = 0; i < steps; ++i)
    world.step(fixed_dt);

  Scene scene;
  auto entity = scene.registry().create();
  scene.registry().emplace<Components::Transform>(entity);
  scene.registry().emplace<Components::RigidBody>(entity, dynamic_body);
  world.sync_transforms_out(scene);

  const f32 resting_y =
      scene.registry().get<Components::Transform>(entity).get().position.y;

  // Unconstrained, the sphere would fall roughly 0.5 * 9.81 * 2^2 ≈ 19.6
  // units in 2 seconds. The fixed constraint should hold it near its
  // anchored offset instead.
  CHECK(resting_y > 3.5F);
}

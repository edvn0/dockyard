#include <dockyard/animation.hpp>
#include <doctest/doctest.h>

using namespace dy;

TEST_CASE("Given a joint chain mirroring the node hierarchy, When parents are "
          "resolved, Then each joint points to its hierarchical parent joint") {
  // Nodes: 0 -> 1 -> 2 (each the child of the previous). All three are joints.
  const std::array<i32, 3> joint_nodes{0, 1, 2};
  const std::array<i32, 3> node_parent{-1, 0, 1};

  const auto parents = resolve_joint_parents(joint_nodes, node_parent);

  REQUIRE(parents.size() == 3);
  CHECK(parents[0] == -1); // node 0 is a root
  CHECK(parents[1] == 0);  // node 1's parent node 0 is joint 0
  CHECK(parents[2] == 1);  // node 2's parent node 1 is joint 1
}

TEST_CASE("Given a non-joint node between two joints, When parents are "
          "resolved, Then the intermediate node is skipped") {
  // Node hierarchy: 0 -> 1 -> 2. Joints are nodes 0 and 2 only; node 1 is a
  // plain transform node that is not part of the skin.
  const std::array<i32, 2> joint_nodes{0, 2};
  const std::array<i32, 3> node_parent{-1, 0, 1};

  const auto parents = resolve_joint_parents(joint_nodes, node_parent);

  REQUIRE(parents.size() == 2);
  CHECK(parents[0] == -1); // joint for node 0 is a root
  CHECK(parents[1] == 0);  // joint for node 2 walks past node 1 to joint 0
}

TEST_CASE("Given joints with no joint ancestors, When parents are resolved, "
          "Then every joint is a root") {
  const std::array<i32, 3> joint_nodes{5, 6, 7};
  const std::array<i32, 8> node_parent{-1, -1, -1, -1, -1, -1, -1, -1};

  const auto parents = resolve_joint_parents(joint_nodes, node_parent);

  REQUIRE(parents.size() == 3);
  CHECK(parents[0] == -1);
  CHECK(parents[1] == -1);
  CHECK(parents[2] == -1);
}

TEST_CASE("Given samplers with differing keyframe ranges, When the clip "
          "duration is computed, Then it is the latest keyframe time") {
  std::vector<AnimationSampler> samplers(2);
  samplers[0].inputs = {0.0F, 0.5F, 1.25F};
  samplers[1].inputs = {0.0F, 2.0F};

  CHECK(compute_clip_duration(samplers) == doctest::Approx(2.0F));
}

TEST_CASE("Given no samplers, When the clip duration is computed, Then it is "
          "zero") {
  CHECK(compute_clip_duration({}) == doctest::Approx(0.0F));
}

// ---------------------------------------------------------------------------
// sample_channel
// ---------------------------------------------------------------------------

TEST_CASE("Given a Step sampler with two keyframes, When sampled between them, "
          "Then the floor keyframe value is returned") {
  AnimationSampler s;
  s.interpolation = AnimationInterpolation::Step;
  s.inputs        = {0.0F, 1.0F};
  s.outputs       = {glm::vec4{1.0F, 0.0F, 0.0F, 0.0F},
                     glm::vec4{0.0F, 1.0F, 0.0F, 0.0F}};

  const auto v = sample_channel(s, AnimationTargetPath::Translation, 0.4F);
  CHECK(v.x == doctest::Approx(1.0F));
  CHECK(v.y == doctest::Approx(0.0F));
}

TEST_CASE("Given a Step sampler, When sampled past the last keyframe, "
          "Then the last value is returned") {
  AnimationSampler s;
  s.interpolation = AnimationInterpolation::Step;
  s.inputs        = {0.0F, 1.0F};
  s.outputs       = {glm::vec4{0.0F}, glm::vec4{5.0F, 0.0F, 0.0F, 0.0F}};

  const auto v = sample_channel(s, AnimationTargetPath::Translation, 2.0F);
  CHECK(v.x == doctest::Approx(5.0F));
}

TEST_CASE("Given a Linear translation sampler, When sampled at 50% between "
          "two keyframes, Then the midpoint value is returned") {
  AnimationSampler s;
  s.interpolation = AnimationInterpolation::Linear;
  s.inputs        = {0.0F, 1.0F};
  s.outputs       = {glm::vec4{0.0F, 0.0F, 0.0F, 0.0F},
                     glm::vec4{2.0F, 4.0F, 0.0F, 0.0F}};

  const auto v = sample_channel(s, AnimationTargetPath::Translation, 0.5F);
  CHECK(v.x == doctest::Approx(1.0F));
  CHECK(v.y == doctest::Approx(2.0F));
}

TEST_CASE("Given a Linear rotation sampler, When sampled at 50% between "
          "identity and 180-degree-Y, Then the result is normalized") {
  // identity quat and a 90-degree rotation around Y (halfway to 180)
  const glm::quat q0 = glm::quat{1.0F, 0.0F, 0.0F, 0.0F}; // identity (w,x,y,z)
  const glm::quat q1 = glm::angleAxis(glm::radians(90.0F), glm::vec3{0, 1, 0});

  AnimationSampler s;
  s.interpolation = AnimationInterpolation::Linear;
  s.inputs        = {0.0F, 1.0F};
  s.outputs       = {glm::vec4{q0.x, q0.y, q0.z, q0.w},
                     glm::vec4{q1.x, q1.y, q1.z, q1.w}};

  const auto v    = sample_channel(s, AnimationTargetPath::Rotation, 0.5F);
  const glm::quat r{v.w, v.x, v.y, v.z};
  CHECK(glm::length(r) == doctest::Approx(1.0F).epsilon(1e-5));
}

TEST_CASE("Given a CubicSpline sampler, When sampled exactly at a keyframe, "
          "Then the keyframe value is returned") {
  // Each keyframe: [in-tangent, value, out-tangent] — all as vec4
  AnimationSampler s;
  s.interpolation = AnimationInterpolation::CubicSpline;
  s.inputs        = {0.0F, 1.0F};
  // outputs layout: [in0, val0, out0, in1, val1, out1]
  s.outputs       = {glm::vec4{0.0F}, glm::vec4{3.0F, 0.0F, 0.0F, 0.0F}, glm::vec4{0.0F},
                     glm::vec4{0.0F}, glm::vec4{7.0F, 0.0F, 0.0F, 0.0F}, glm::vec4{0.0F}};

  CHECK(sample_channel(s, AnimationTargetPath::Translation, 0.0F).x == doctest::Approx(3.0F));
  CHECK(sample_channel(s, AnimationTargetPath::Translation, 1.0F).x == doctest::Approx(7.0F));
}

// ---------------------------------------------------------------------------
// compute_joint_palette
// ---------------------------------------------------------------------------

TEST_CASE("Given a single root joint with identity transforms and no animation "
          "channels, When the joint palette is computed, Then it equals the "
          "inverse-bind matrix") {
  Skeleton sk;
  SkeletonJoint j;
  j.parent            = -1;
  j.bind_translation  = {0.0F, 0.0F, 0.0F};
  j.bind_rotation     = glm::quat{1.0F, 0.0F, 0.0F, 0.0F};
  j.bind_scale        = {1.0F, 1.0F, 1.0F};
  j.inverse_bind      = glm::translate(glm::mat4{1.0F}, glm::vec3{-5.0F, 0.0F, 0.0F});
  sk.joints.push_back(j);

  AnimationClip clip;
  clip.duration = 1.0F;

  const auto palette = compute_joint_palette(sk, clip, 0, 0.5F);

  REQUIRE(palette.size() == 1);
  // world[0] = identity (bind pose), so palette[0] = identity * inverse_bind = inverse_bind
  CHECK(palette[0][3][0] == doctest::Approx(-5.0F));
}

TEST_CASE("Given a two-joint chain with a translation channel on the parent, "
          "When the joint palette is computed, Then the child inherits the "
          "parent world transform") {
  Skeleton sk;

  SkeletonJoint root;
  root.parent           = -1;
  root.node_index       = 0;
  root.bind_translation = {0.0F, 0.0F, 0.0F};
  root.bind_rotation    = glm::quat{1.0F, 0.0F, 0.0F, 0.0F};
  root.bind_scale       = {1.0F, 1.0F, 1.0F};
  root.inverse_bind     = glm::mat4{1.0F};

  SkeletonJoint child;
  child.parent           = 0;
  child.node_index       = 1;
  child.bind_translation = {0.0F, 0.0F, 0.0F};
  child.bind_rotation    = glm::quat{1.0F, 0.0F, 0.0F, 0.0F};
  child.bind_scale       = {1.0F, 1.0F, 1.0F};
  child.inverse_bind     = glm::mat4{1.0F};

  sk.joints.push_back(root);
  sk.joints.push_back(child);

  // Animation: move root to (3, 0, 0)
  AnimationSampler sampler;
  sampler.interpolation = AnimationInterpolation::Step;
  sampler.inputs        = {0.0F};
  sampler.outputs       = {glm::vec4{3.0F, 0.0F, 0.0F, 0.0F}};

  AnimationChannel ch;
  ch.skeleton_index = 0;
  ch.joint_index    = 0;
  ch.path           = AnimationTargetPath::Translation;
  ch.sampler_index  = 0;

  AnimationClip clip;
  clip.duration = 1.0F;
  clip.samplers.push_back(sampler);
  clip.channels.push_back(ch);

  const auto palette = compute_joint_palette(sk, clip, 0, 0.0F);

  REQUIRE(palette.size() == 2);
  // root world = translate(3,0,0), palette[0] = translate(3,0,0) * identity
  CHECK(palette[0][3][0] == doctest::Approx(3.0F));
  // child world = root_world * identity = translate(3,0,0), palette[1] = same
  CHECK(palette[1][3][0] == doctest::Approx(3.0F));
}

// ---------------------------------------------------------------------------
// AnimationState
// ---------------------------------------------------------------------------

TEST_CASE("Given an AnimationState with a looping clip, When advanced past the "
          "clip duration, Then time wraps and the palette is populated") {
  Skeleton sk;
  SkeletonJoint j;
  j.parent        = -1;
  j.bind_rotation = glm::quat{1.0F, 0.0F, 0.0F, 0.0F};
  j.bind_scale    = {1.0F, 1.0F, 1.0F};
  j.inverse_bind  = glm::mat4{1.0F};
  sk.joints.push_back(j);

  AnimationClip clip;
  clip.duration = 2.0F;

  AnimationState state;
  state.skeleton        = &sk;
  state.clip            = &clip;
  state.skeleton_index  = 0;
  state.loop            = true;

  state.advance(3.5F); // 3.5 mod 2.0 = 1.5
  CHECK(state.time == doctest::Approx(1.5F).epsilon(1e-5));
  REQUIRE(state.joint_palette.size() == 1);
}

TEST_CASE("Given an AnimationState with looping disabled, When advanced past "
          "the clip duration, Then time clamps to duration") {
  Skeleton sk;
  SkeletonJoint j;
  j.parent        = -1;
  j.bind_rotation = glm::quat{1.0F, 0.0F, 0.0F, 0.0F};
  j.bind_scale    = {1.0F, 1.0F, 1.0F};
  j.inverse_bind  = glm::mat4{1.0F};
  sk.joints.push_back(j);

  AnimationClip clip;
  clip.duration = 1.0F;

  AnimationState state;
  state.skeleton        = &sk;
  state.clip            = &clip;
  state.skeleton_index  = 0;
  state.loop            = false;

  state.advance(5.0F);
  CHECK(state.time == doctest::Approx(1.0F));
}

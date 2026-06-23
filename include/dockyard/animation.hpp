#pragma once

#include <dockyard/types.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <span>
#include <string>
#include <vector>

namespace dy {

struct SkeletonJoint {
  i32 parent = -1;
  i32 node_index = -1;
  glm::mat4 inverse_bind{1.0F};

  glm::vec3 bind_translation{0.0F};
  glm::quat bind_rotation{1.0F, 0.0F, 0.0F, 0.0F};
  glm::vec3 bind_scale{1.0F};

  std::string name;
};

struct Skeleton {
  std::vector<SkeletonJoint> joints;

  [[nodiscard]] auto joint_count() const -> u32 {
    return static_cast<u32>(joints.size());
  }
};

enum class AnimationInterpolation : u8 { Linear, Step, CubicSpline };
enum class AnimationTargetPath : u8 { Translation, Rotation, Scale };

struct AnimationSampler {
  std::vector<f32> inputs;
  std::vector<glm::vec4> outputs;
  AnimationInterpolation interpolation = AnimationInterpolation::Linear;
};

struct AnimationChannel {
  i32 skeleton_index = -1;
  i32 joint_index = -1;
  AnimationTargetPath path = AnimationTargetPath::Translation;
  u32 sampler_index = 0;
};

struct AnimationClip {
  std::string name;
  f32 duration = 0.0F;
  std::vector<AnimationSampler> samplers;
  std::vector<AnimationChannel> channels;
};

struct AnimationTransform {
  glm::vec3 translation{0.0F};
  glm::quat rotation{1.0F, 0.0F, 0.0F, 0.0F};
  glm::vec3 scale{1.0F};
};

[[nodiscard]] auto resolve_joint_parents(std::span<const i32> joint_nodes,
                                         std::span<const i32> node_parent)
    -> std::vector<i32>;

[[nodiscard]] auto compute_clip_duration(std::span<const AnimationSampler>)
    -> f32;

[[nodiscard]] auto sample_channel(const AnimationSampler &sampler,
                                  AnimationTargetPath path, f32 t, u32 &cursor)
    -> glm::vec4;

struct AnimationState {
  const Skeleton *skeleton = nullptr;
  const AnimationClip *clip = nullptr;
  i32 skeleton_index = 0;
  f32 time = 0.0F;
  bool loop = true;

  std::vector<AnimationTransform> local_transforms;
  std::vector<glm::mat4> world_transforms;
  std::vector<glm::mat4> joint_palette;

  std::vector<u32> channel_cursors;

  static auto create(const Skeleton *tgt_skel, const AnimationClip *tgt_clip,
                     i32 skel_idx = 0) -> AnimationState {
    AnimationState s{};
    s.init(tgt_skel, tgt_clip, skel_idx);
    s.advance(0.0F);
    return s;
  }

  auto update_animation(const AnimationClip &) -> void;
  auto advance(f32 dt) -> void;

private:
  auto init(const Skeleton *target_skeleton, const AnimationClip *target_clip,
            i32 skel_idx) -> void;
};

auto compute_joint_palette(AnimationState &state) -> void;

} // namespace dy